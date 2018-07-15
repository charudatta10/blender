/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file view3d_gizmo_navigate_type.c
 *  \ingroup wm
 *
 * \name Custom Orientation/Navigation Gizmo for the 3D View
 *
 * \brief Simple gizmo to axis and translate.
 *
 * - scale_basis: used for the size.
 * - matrix_basis: used for the location.
 * - matrix_offset: used to store the orientation.
 */

#include "MEM_guardedalloc.h"

#include "BLI_math.h"
#include "BLI_sort_utils.h"

#include "BKE_context.h"

#include "BIF_gl.h"
#include "BIF_glutil.h"

#include "GPU_immediate.h"
#include "GPU_immediate_util.h"
#include "GPU_matrix.h"
#include "GPU_state.h"
#include "GPU_batch.h"
#include "GPU_batch_presets.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "WM_api.h"
#include "WM_types.h"

#include "ED_screen.h"

#include "view3d_intern.h"

#define DIAL_RESOLUTION 32

#define HANDLE_SIZE 0.33

/**
 * \param viewmat_local_unit is typically the 'rv3d->viewmatob'
 * copied into a 3x3 matrix and normalized.
 */
static void draw_xyz_wire(
        uint pos_id, const float viewmat_local_unit[3][3], const float c[3], float size, int axis)
{
	int line_type;
	float buffer[4][3];
	int n = 0;

	float v1[3] = {0.0f, 0.0f, 0.0f}, v2[3] = {0.0f, 0.0f, 0.0f};
	float dim = size * 0.1f;
	float dx[3], dy[3];

	dx[0] = dim;  dx[1] = 0.0f; dx[2] = 0.0f;
	dy[0] = 0.0f; dy[1] = dim;  dy[2] = 0.0f;

	switch (axis) {
		case 0:     /* x axis */
			line_type = GWN_PRIM_LINES;

			/* bottom left to top right */
			negate_v3_v3(v1, dx);
			sub_v3_v3(v1, dy);
			copy_v3_v3(v2, dx);
			add_v3_v3(v2, dy);

			copy_v3_v3(buffer[n++], v1);
			copy_v3_v3(buffer[n++], v2);

			/* top left to bottom right */
			mul_v3_fl(dy, 2.0f);
			add_v3_v3(v1, dy);
			sub_v3_v3(v2, dy);

			copy_v3_v3(buffer[n++], v1);
			copy_v3_v3(buffer[n++], v2);

			break;
		case 1:     /* y axis */
			line_type = GWN_PRIM_LINES;

			/* bottom left to top right */
			mul_v3_fl(dx, 0.75f);
			negate_v3_v3(v1, dx);
			sub_v3_v3(v1, dy);
			copy_v3_v3(v2, dx);
			add_v3_v3(v2, dy);

			copy_v3_v3(buffer[n++], v1);
			copy_v3_v3(buffer[n++], v2);

			/* top left to center */
			mul_v3_fl(dy, 2.0f);
			add_v3_v3(v1, dy);
			zero_v3(v2);

			copy_v3_v3(buffer[n++], v1);
			copy_v3_v3(buffer[n++], v2);

			break;
		case 2:     /* z axis */
			line_type = GWN_PRIM_LINE_STRIP;

			/* start at top left */
			negate_v3_v3(v1, dx);
			add_v3_v3(v1, dy);

			copy_v3_v3(buffer[n++], v1);

			mul_v3_fl(dx, 2.0f);
			add_v3_v3(v1, dx);

			copy_v3_v3(buffer[n++], v1);

			mul_v3_fl(dy, 2.0f);
			sub_v3_v3(v1, dx);
			sub_v3_v3(v1, dy);

			copy_v3_v3(buffer[n++], v1);

			add_v3_v3(v1, dx);

			copy_v3_v3(buffer[n++], v1);

			break;
		default:
			BLI_assert(0);
			return;
	}

	for (int i = 0; i < n; i++) {
		mul_transposed_m3_v3((float (*)[3])viewmat_local_unit, buffer[i]);
		add_v3_v3(buffer[i], c);
	}

	immBegin(line_type, n);
	for (int i = 0; i < n; i++) {
		immVertex3fv(pos_id, buffer[i]);
	}
	immEnd();
}

static void axis_geom_draw(const wmGizmo *mpr, const float color[4], const bool UNUSED(select))
{
	GPU_line_width(mpr->line_width);

	Gwn_VertFormat *format = immVertexFormat();
	const uint pos_id = GWN_vertformat_attr_add(format, "pos", GWN_COMP_F32, 3, GWN_FETCH_FLOAT);
	immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

	struct {
		float depth;
		char index;
		char axis;
		char is_pos;
	} axis_order[6] = {
		{-mpr->matrix_offset[0][2], 0, 0, false},
		{+mpr->matrix_offset[0][2], 1, 0, true},
		{-mpr->matrix_offset[1][2], 2, 1, false},
		{+mpr->matrix_offset[1][2], 3, 1, true},
		{-mpr->matrix_offset[2][2], 4, 2, false},
		{+mpr->matrix_offset[2][2], 5, 2, true},
	};
	qsort(&axis_order, ARRAY_SIZE(axis_order), sizeof(axis_order[0]), BLI_sortutil_cmp_float);

	const float scale_axis = 0.25f;
	static const float axis_highlight[4] = {1, 1, 1, 1};
	static const float axis_black[4] = {0, 0, 0, 1};
	static float axis_color[3][4];
	gpuPushMatrix();
	gpuMultMatrix(mpr->matrix_offset);

	bool draw_center_done = false;

	int axis_align = -1;
	for (int axis = 0; axis < 3; axis++) {
		if (len_squared_v2(mpr->matrix_offset[axis]) < 1e-6f) {
			axis_align = axis;
			break;
		}
	}

	for (int axis_index = 0; axis_index < ARRAY_SIZE(axis_order); axis_index++) {
		const int index = axis_order[axis_index].index;
		const int axis = axis_order[axis_index].axis;
		const bool is_pos = axis_order[axis_index].is_pos;
		const bool is_highlight = index + 1 == mpr->highlight_part;

		/* Draw slightly before, so axis aligned arrows draw ontop. */
		if ((draw_center_done == false) && (axis_order[axis_index].depth > -0.01f)) {

			/* Circle defining active area (revert back to 2D space). */
			{
				gpuPopMatrix();
				immUniformColor4fv(color);
				imm_draw_circle_fill_3d(pos_id, 0, 0, 1.0f, DIAL_RESOLUTION);
				gpuPushMatrix();
				gpuMultMatrix(mpr->matrix_offset);
			}
			draw_center_done = true;
		}
		UI_GetThemeColor3fv(TH_AXIS_X + axis, axis_color[axis]);
		axis_color[axis][3] = 1.0f;

		const int index_z = axis;
		const int index_y = (axis + 1) % 3;
		const int index_x = (axis + 2) % 3;

		bool ok = true;

		/* skip view align axis */
		if ((axis_align == axis) && (mpr->matrix_offset[axis][2] > 0.0f) == is_pos) {
			ok = false;
		}
		if (ok) {
			const float v[3] = {0, 0, 3 * (is_pos ? 1 : -1)};
			const float v_final[3] = {
				v[index_x] * scale_axis,
				v[index_y] * scale_axis,
				v[index_z] * scale_axis,
			};
			const float *color_current = is_highlight ? axis_highlight : axis_color[axis];
			float color_current_fade[4];
			copy_v4_v4(color_current_fade, color_current);
			color_current_fade[3] *= 0.2;

			/* Axis Line. */
			if (is_pos) {
				float v_start[3];
				GPU_line_width(2.0f);
				immUniformColor4fv(color_current);
				immBegin(GWN_PRIM_LINES, 2);
				if (axis_align == -1) {
					zero_v3(v_start);
				}
				else {
					/* When axis aligned we don't draw the front most axis
					 * (allowing us to switch to the opposite side).
					 * In this case don't draw lines over axis pointing away from us
					 * because it obscures character and looks noisy.
					 */
					mul_v3_v3fl(v_start, v_final, 0.3f);
				}
				immVertex3fv(pos_id, v_start);
				immVertex3fv(pos_id, v_final);
				immEnd();
			}

			/* Axis Ball. */
			{
				gpuPushMatrix();
				gpuTranslate3fv(v_final);
				gpuScaleUniform(is_pos ? 0.22f : 0.18f);

				Gwn_Batch *sphere = GPU_batch_preset_sphere(0);
				GWN_batch_program_set_builtin(sphere, GPU_SHADER_3D_UNIFORM_COLOR);
				GWN_batch_uniform_4fv(sphere, "color", is_pos ? color_current : color_current_fade);
				GWN_batch_draw(sphere);
				gpuPopMatrix();
			}

			/* Axis XYZ Character. */
			if (is_pos) {
				GPU_line_width(1.0f);
				float m3[3][3];
				copy_m3_m4(m3, mpr->matrix_offset);
				immUniformColor4fv(is_highlight ? axis_black : axis_highlight);
				draw_xyz_wire(pos_id, m3, v_final, 1.0, axis);
			}
		}
	}

	gpuPopMatrix();
	immUnbindProgram();
}

static void axis3d_draw_intern(
        const bContext *UNUSED(C), wmGizmo *mpr,
        const bool select, const bool highlight)
{
	const float *color = highlight ? mpr->color_hi : mpr->color;
	float matrix_final[4][4];
	float matrix_unit[4][4];

	unit_m4(matrix_unit);

	WM_gizmo_calc_matrix_final_params(
	        mpr,
	        &((struct WM_GizmoMatrixParams) {
	            .matrix_offset = matrix_unit,
	        }), matrix_final);

	gpuPushMatrix();
	gpuMultMatrix(matrix_final);

	GPU_blend(true);
	axis_geom_draw(mpr, color, select);
	GPU_blend(false);
	gpuPopMatrix();
}

static void gizmo_axis_draw(const bContext *C, wmGizmo *mpr)
{
	const bool is_modal = mpr->state & WM_GIZMO_STATE_MODAL;
	const bool is_highlight = (mpr->state & WM_GIZMO_STATE_HIGHLIGHT) != 0;

	(void)is_modal;

	GPU_blend(true);
	axis3d_draw_intern(C, mpr, false, is_highlight);
	GPU_blend(false);
}

static int gizmo_axis_test_select(
        bContext *UNUSED(C), wmGizmo *mpr, const wmEvent *event)
{
	float point_local[2] = {UNPACK2(event->mval)};
	sub_v2_v2(point_local, mpr->matrix_basis[3]);
	mul_v2_fl(point_local, 1.0f / (mpr->scale_basis * UI_DPI_FAC));

	const float len_sq = len_squared_v2(point_local);
	if (len_sq > 1.0) {
		return -1;
	}

	int part_best = -1;
	int part_index = 1;
	/* Use 'SQUARE(HANDLE_SIZE)' if we want to be able to _not_ focus on one of the axis. */
	float i_best_len_sq = FLT_MAX;
	for (int i = 0; i < 3; i++) {
		for (int is_pos = 0; is_pos < 2; is_pos++) {
			float co[2] = {
				mpr->matrix_offset[i][0] * (is_pos ? 1 : -1),
				mpr->matrix_offset[i][1] * (is_pos ? 1 : -1),
			};

			bool ok = true;

			/* Check if we're viewing on an axis, there is no point to clicking on the current axis so show the reverse. */
			if (len_squared_v2(co) < 1e-6f && (mpr->matrix_offset[i][2] > 0.0f) == is_pos) {
				ok = false;
			}

			if (ok) {
				const float len_axis_sq = len_squared_v2v2(co, point_local);
				if (len_axis_sq < i_best_len_sq) {
					part_best = part_index;
					i_best_len_sq = len_axis_sq;
				}
			}
			part_index += 1;
		}
	}

	if (part_best != -1) {
		return part_best;
	}

	/* The 'mpr->scale_final' is already applied when projecting. */
	if (len_sq < 1.0f) {
		return 0;
	}

	return -1;
}

static int gizmo_axis_cursor_get(wmGizmo *mpr)
{
	if (mpr->highlight_part > 0) {
		return CURSOR_EDIT;
	}
	return BC_NSEW_SCROLLCURSOR;
}

void VIEW3D_WT_navigate_rotate(wmGizmoType *wt)
{
	/* identifiers */
	wt->idname = "VIEW3D_WT_navigate_rotate";

	/* api callbacks */
	wt->draw = gizmo_axis_draw;
	wt->test_select = gizmo_axis_test_select;
	wt->cursor_get = gizmo_axis_cursor_get;

	wt->struct_size = sizeof(wmGizmo);
}