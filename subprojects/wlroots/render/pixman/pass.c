#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include "render/pixman.h"

static const struct wlr_render_pass_impl render_pass_impl;

static struct wlr_pixman_render_pass *get_render_pass(struct wlr_render_pass *wlr_pass) {
	assert(wlr_pass->impl == &render_pass_impl);
	struct wlr_pixman_render_pass *pass = wl_container_of(wlr_pass, pass, base);
	return pass;
}

static struct wlr_pixman_texture *get_texture(struct wlr_texture *wlr_texture) {
	assert(wlr_texture_is_pixman(wlr_texture));
	struct wlr_pixman_texture *texture = wl_container_of(wlr_texture, texture, wlr_texture);
	return texture;
}

static bool render_pass_submit(struct wlr_render_pass *wlr_pass) {
	struct wlr_pixman_render_pass *pass = get_render_pass(wlr_pass);

	wlr_buffer_end_data_ptr_access(pass->buffer->buffer);
	wlr_buffer_unlock(pass->buffer->buffer);
	free(pass);

	return true;
}

static pixman_op_t get_pixman_blending(enum wlr_render_blend_mode mode) {
	switch (mode) {
	case WLR_RENDER_BLEND_MODE_PREMULTIPLIED:
		return PIXMAN_OP_OVER;
	case WLR_RENDER_BLEND_MODE_NONE:
		return PIXMAN_OP_SRC;
	}
	abort();
}

static void render_pass_add_texture(struct wlr_render_pass *wlr_pass,
		const struct wlr_render_texture_options *options) {
	struct wlr_pixman_render_pass *pass = get_render_pass(wlr_pass);
	struct wlr_pixman_texture *texture = get_texture(options->texture);
	struct wlr_pixman_buffer *buffer = pass->buffer;

	if (texture->buffer != NULL && !begin_pixman_data_ptr_access(texture->buffer,
			&texture->image, WLR_BUFFER_DATA_PTR_ACCESS_READ)) {
		return;
	}

	pixman_op_t op = get_pixman_blending(options->blend_mode);
	pixman_image_set_clip_region32(buffer->image, options->clip);

	struct wlr_fbox src_fbox;
	wlr_render_texture_options_get_src_box(options, &src_fbox);
	struct wlr_box src_box = {
		.x = roundf(src_fbox.x),
		.y = roundf(src_fbox.y),
		.width = roundf(src_fbox.width),
		.height = roundf(src_fbox.height),
	};

	struct wlr_box dst_box;
	wlr_render_texture_options_get_dst_box(options, &dst_box);

	pixman_image_t *mask = NULL;
	float alpha = wlr_render_texture_options_get_alpha(options);
	if (alpha != 1) {
		mask = pixman_image_create_solid_fill(&(struct pixman_color){
			.alpha = 0xFFFF * alpha,
		});
	}

	// Rotate the source size into destination coordinates
	struct wlr_box src_box_transformed;
	wlr_box_transform(&src_box_transformed, &src_box, options->transform,
		buffer->buffer->width, buffer->buffer->height);

	if (options->transform != WL_OUTPUT_TRANSFORM_NORMAL ||
			src_box_transformed.width != dst_box.width ||
			src_box_transformed.height != dst_box.height) {
		// Cosinus/sinus values are exact integers for enum wl_output_transform entries
		int tr_cos = 1, tr_sin = 0, tr_x = 0, tr_y = 0;
		switch (options->transform) {
		case WL_OUTPUT_TRANSFORM_NORMAL:
		case WL_OUTPUT_TRANSFORM_FLIPPED:
			break;
		case WL_OUTPUT_TRANSFORM_90:
		case WL_OUTPUT_TRANSFORM_FLIPPED_90:
			tr_cos = 0;
			tr_sin = 1;
			tr_y = src_box.width;
			break;
		case WL_OUTPUT_TRANSFORM_180:
		case WL_OUTPUT_TRANSFORM_FLIPPED_180:
			tr_cos = -1;
			tr_sin = 0;
			tr_x = src_box.width;
			tr_y = src_box.height;
			break;
		case WL_OUTPUT_TRANSFORM_270:
		case WL_OUTPUT_TRANSFORM_FLIPPED_270:
			tr_cos = 0;
			tr_sin = -1;
			tr_x = src_box.height;
			break;
		}

		// Pixman transforms are generally the opposite of what you expect because they
		// apply to the coordinate system rather than the image.  The comments here
		// refer to what happens to the image, so all the code between
		// pixman_transform_init_identity() and pixman_image_set_transform() is probably
		// best read backwards.  Also this means translations are in the opposite
		// direction, imagine them as moving the origin around rather than moving the
		// image.
		//
		// Beware that this doesn't work quite the same as wp_viewporter: We apply crop
		// before transform and scale, whereas it defines crop in post-transform-scale
		// coordinates.  But this only applies to internal wlroots code - the viewporter
		// extension code makes sure that to clients everything works as it should.

		struct pixman_transform transform;
		pixman_transform_init_identity(&transform);

		// Apply scaling to get to the dst_box size.  Because the scaling is applied last
		// it depends on the whether the rotation swapped width and height, which is why
		// we use src_box_transformed instead of src_box.
		pixman_transform_scale(&transform, NULL,
			pixman_double_to_fixed(src_box_transformed.width / (double)dst_box.width),
			pixman_double_to_fixed(src_box_transformed.height / (double)dst_box.height));

		// pixman rotates about the origin which again leaves everything outside of the
		// viewport.  Translate the result so that its new top-left corner is back at the
		// origin.
		pixman_transform_translate(&transform, NULL,
			-pixman_int_to_fixed(tr_x), -pixman_int_to_fixed(tr_y));

		// Apply the rotation
		pixman_transform_rotate(&transform, NULL,
			pixman_int_to_fixed(tr_cos), pixman_int_to_fixed(tr_sin));

		// Apply flip before rotation
		if (options->transform >= WL_OUTPUT_TRANSFORM_FLIPPED) {
			// The flip leaves everything left of the Y axis which is outside the
			// viewport. So translate everything back into the viewport.
			pixman_transform_translate(&transform, NULL,
				-pixman_int_to_fixed(src_box.width), pixman_int_to_fixed(0));
			// Flip by applying a scale of -1 to the X axis
			pixman_transform_scale(&transform, NULL,
				pixman_int_to_fixed(-1), pixman_int_to_fixed(1));
		}

		// Apply the translation for source crop so the origin is now at the top-left of
		// the region we're actually using.  Do this last so all the other transforms
		// apply on top of this.
		pixman_transform_translate(&transform, NULL,
			pixman_int_to_fixed(src_box.x), pixman_int_to_fixed(src_box.y));

		pixman_image_set_transform(texture->image, &transform);

		switch (options->filter_mode) {
		case WLR_SCALE_FILTER_BILINEAR:
			pixman_image_set_repeat(texture->image, PIXMAN_REPEAT_PAD);
			pixman_image_set_filter(texture->image, PIXMAN_FILTER_BILINEAR, NULL, 0);
			break;
		case WLR_SCALE_FILTER_NEAREST:
			pixman_image_set_filter(texture->image, PIXMAN_FILTER_NEAREST, NULL, 0);
			break;
		}

		// Now composite the result onto the pass buffer.  We specify a source origin of 0,0
		// because the x,y part of source crop is already done using the transform. The
		// width,height part of source crop is done here by the width and height we pass:
		// because of the scaling, cropping at the end by dst_box.{width,height} is
		// equivalent to if we cropped at the start by src_box.{width,height}.
		pixman_image_composite32(op, texture->image, mask, buffer->image,
			0, 0, // source x,y
			0, 0, // mask x,y
			dst_box.x, dst_box.y, // dest x,y
			dst_box.width, dst_box.height // composite width,height
		);

		pixman_image_set_transform(texture->image, NULL);
	} else {
		// No transforms or crop needed, just a straight blit from the source
		pixman_image_set_transform(texture->image, NULL);
		pixman_image_composite32(op, texture->image, mask, buffer->image,
			src_box.x, src_box.y, 0, 0, dst_box.x, dst_box.y,
			src_box.width, src_box.height);
	}

	pixman_image_set_clip_region32(buffer->image, NULL);

	if (texture->buffer != NULL) {
		wlr_buffer_end_data_ptr_access(texture->buffer);
	}

	if (mask != NULL) {
		pixman_image_unref(mask);
	}
}

static void render_pass_add_rect(struct wlr_render_pass *wlr_pass,
		const struct wlr_render_rect_options *options) {
	struct wlr_pixman_render_pass *pass = get_render_pass(wlr_pass);
	struct wlr_pixman_buffer *buffer = pass->buffer;
	struct wlr_box box;
	wlr_render_rect_options_get_box(options, pass->buffer->buffer, &box);

	pixman_op_t op = get_pixman_blending(options->color.a == 1 ?
		WLR_RENDER_BLEND_MODE_NONE : options->blend_mode);

	struct pixman_color color = {
		.red = options->color.r * 0xFFFF,
		.green = options->color.g * 0xFFFF,
		.blue = options->color.b * 0xFFFF,
		.alpha = options->color.a * 0xFFFF,
	};

	pixman_image_t *fill = pixman_image_create_solid_fill(&color);

	pixman_image_set_clip_region32(buffer->image, options->clip);
	pixman_image_composite32(op, fill, NULL, buffer->image,
		0, 0, 0, 0, box.x, box.y, box.width, box.height);
	pixman_image_set_clip_region32(buffer->image, NULL);

	pixman_image_unref(fill);
}

// scroll-secure: the pixman renderer drew no decorations at all, so with
// pixman (headless, VMs without a GPU) windows had no borders or title bar
// background. Fill the same areas as the gles2 decoration shader, with
// square corners (border_radius is ignored) and no antialiasing.
static void decoration_fill(struct wlr_pixman_render_pass *pass,
		const struct wlr_render_decoration_options *options,
		double lx, double ly, double lw, double lh,
		const struct wlr_render_color *rc) {
	if (rc->a <= 0 || lw <= 0 || lh <= 0) {
		return;
	}
	const struct wlr_box *box = &options->box;
	// Logical (untransformed) rect to buffer coordinates, the inverse of the
	// shader's flip/swap of gl_FragCoord
	double bx = lx, by = ly, bw = lw, bh = lh;
	if (options->swap_xy) {
		bx = ly;
		by = lx;
		bw = lh;
		bh = lw;
	}
	if (options->flip_x) {
		bx = box->width - bx - bw;
	}
	if (options->flip_y) {
		by = box->height - by - bh;
	}
	int x0 = box->x + (int)round(bx), y0 = box->y + (int)round(by);
	int x1 = box->x + (int)round(bx + bw), y1 = box->y + (int)round(by + bh);
	if (x1 <= x0 || y1 <= y0) {
		return;
	}

	struct pixman_color color = {
		.red = rc->r * 0xFFFF,
		.green = rc->g * 0xFFFF,
		.blue = rc->b * 0xFFFF,
		.alpha = rc->a * 0xFFFF,
	};
	pixman_op_t op = get_pixman_blending(rc->a == 1 ?
		WLR_RENDER_BLEND_MODE_NONE : options->blend_mode);
	pixman_image_t *fill = pixman_image_create_solid_fill(&color);
	pixman_image_set_clip_region32(pass->buffer->image, options->clip);
	pixman_image_composite32(op, fill, NULL, pass->buffer->image,
		0, 0, 0, 0, x0, y0, x1 - x0, y1 - y0);
	pixman_image_set_clip_region32(pass->buffer->image, NULL);
	pixman_image_unref(fill);
}

static void render_pass_add_decoration(struct wlr_render_pass *wlr_pass,
		const struct wlr_render_decoration_options *options) {
	struct wlr_pixman_render_pass *pass = get_render_pass(wlr_pass);
	double width = options->box.width, height = options->box.height;
	if (options->swap_xy) {
		width = options->box.height;
		height = options->box.width;
	}

	double top = 0;
	if (options->title_bar) {
		top = fmin(options->title_bar_height, height);
		decoration_fill(pass, options, 0, 0, width, top,
			&options->title_bar_color);
	}
	double h = height - top;
	double bw = options->border ? fmin(options->border_width, fmin(width, h) / 2) : 0;
	if (bw > 0) {
		// Left and right own the corners, like the shader
		decoration_fill(pass, options, 0, top, bw, h,
			&options->border_left_color);
		decoration_fill(pass, options, width - bw, top, bw, h,
			&options->border_right_color);
		decoration_fill(pass, options, bw, top, width - 2 * bw, bw,
			&options->border_top_color);
		decoration_fill(pass, options, bw, top + h - bw, width - 2 * bw, bw,
			&options->border_bottom_color);
	}
	if (options->dim) {
		decoration_fill(pass, options, bw, top + bw, width - 2 * bw,
			h - 2 * bw, &options->dim_color);
	}
}

static void render_pass_add_shadow(struct wlr_render_pass *wlr_pass,
		const struct wlr_render_shadow_options *options) {
}

static const struct wlr_render_pass_impl render_pass_impl = {
	.submit = render_pass_submit,
	.add_texture = render_pass_add_texture,
	.add_rect = render_pass_add_rect,
	.add_decoration = render_pass_add_decoration,
	.add_shadow = render_pass_add_shadow,
};

struct wlr_pixman_render_pass *begin_pixman_render_pass(
		struct wlr_pixman_buffer *buffer) {
	struct wlr_pixman_render_pass *pass = calloc(1, sizeof(*pass));
	if (pass == NULL) {
		return NULL;
	}

	wlr_render_pass_init(&pass->base, &render_pass_impl);

	if (!begin_pixman_data_ptr_access(buffer->buffer, &buffer->image,
			WLR_BUFFER_DATA_PTR_ACCESS_READ | WLR_BUFFER_DATA_PTR_ACCESS_WRITE)) {
		free(pass);
		return NULL;
	}

	wlr_buffer_lock(buffer->buffer);
	pass->buffer = buffer;

	return pass;
}
