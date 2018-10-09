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
 * The Original Code is Copyright (C) 2008 Blender Foundation.
 * All rights reserved.
 *
 *
 * Contributor(s): Blender Foundation
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/editors/util/ed_util.c
 *  \ingroup edutil
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "MEM_guardedalloc.h"

#include "DNA_armature_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_scene_types.h"
#include "DNA_packedFile_types.h"

#include "BLI_utildefines.h"
#include "BLI_string.h"
#include "BLI_path_util.h"

#include "BIF_gl.h"
#include "BIF_glutil.h"

#include "BLT_translation.h"

#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_main.h"
#include "BKE_multires.h"
#include "BKE_object.h"
#include "BKE_packedFile.h"
#include "BKE_paint.h"
#include "BKE_screen.h"
#include "BKE_workspace.h"
#include "BKE_layer.h"
#include "BKE_undo_system.h"

#include "ED_armature.h"
#include "ED_buttons.h"
#include "ED_image.h"
#include "ED_mesh.h"
#include "ED_node.h"
#include "ED_object.h"
#include "ED_outliner.h"
#include "ED_paint.h"
#include "ED_space_api.h"
#include "ED_util.h"

#include "GPU_immediate.h"
#include "GPU_state.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "WM_types.h"
#include "WM_api.h"
#include "RNA_access.h"



/* ********* general editor util funcs, not BKE stuff please! ********* */

void ED_editors_init(bContext *C)
{
	Main *bmain = CTX_data_main(C);
	wmWindowManager *wm = CTX_wm_manager(C);

	if (wm->undo_stack == NULL) {
		wm->undo_stack = BKE_undosys_stack_create();
	}

	/* This is called during initialization, so we don't want to store any reports */
	ReportList *reports = CTX_wm_reports(C);
	int reports_flag_prev = reports->flag & ~RPT_STORE;

	SWAP(int, reports->flag, reports_flag_prev);

	/* toggle on modes for objects that were saved with these enabled. for
	 * e.g. linked objects we have to ensure that they are actually the
	 * active object in this scene. */
	Object *obact = CTX_data_active_object(C);
	if (obact != NULL) {
		for (Object *ob = bmain->object.first; ob; ob = ob->id.next) {
			int mode = ob->mode;

			if (mode == OB_MODE_OBJECT) {
				/* pass */
			}
			else if (!BKE_object_has_mode_data(ob, mode)) {
				/* For multi-edit mode we may already have mode data.
				 * (grease pencil does not need it)
				 */
				if (ob->type != OB_GPENCIL) {
					ID *data = ob->data;
					ob->mode = OB_MODE_OBJECT;
					if ((ob == obact) && !ID_IS_LINKED(ob) && !(data && ID_IS_LINKED(data))) {
						ED_object_mode_toggle(C, mode);
					}
				}
			}
		}
	}

	/* image editor paint mode */
	{
		Scene *sce = CTX_data_scene(C);
		if (sce) {
			ED_space_image_paint_update(bmain, wm, sce);
		}
	}

	SWAP(int, reports->flag, reports_flag_prev);
}

/* frees all editmode stuff */
void ED_editors_exit(bContext *C)
{
	Main *bmain = CTX_data_main(C);

	if (!bmain)
		return;

	/* frees all editmode undos */
	if (G_MAIN->wm.first) {
		wmWindowManager *wm = G_MAIN->wm.first;
		/* normally we don't check for NULL undo stack, do here since it may run in different context. */
		if (wm->undo_stack) {
			BKE_undosys_stack_destroy(wm->undo_stack);
			wm->undo_stack = NULL;
		}
	}

	for (Object *ob = bmain->object.first; ob; ob = ob->id.next) {
		if (ob->type == OB_MESH) {
			Mesh *me = ob->data;
			if (me->edit_btmesh) {
				EDBM_mesh_free(me->edit_btmesh);
				MEM_freeN(me->edit_btmesh);
				me->edit_btmesh = NULL;
			}
		}
		else if (ob->type == OB_ARMATURE) {
			bArmature *arm = ob->data;
			if (arm->edbo) {
				ED_armature_edit_free(ob->data);
			}
		}
	}

	/* global in meshtools... */
	ED_mesh_mirror_spatial_table(NULL, NULL, NULL, NULL, 'e');
	ED_mesh_mirror_topo_table(NULL, NULL, 'e');
}

/* flush any temp data from object editing to DNA before writing files,
 * rendering, copying, etc. */
bool ED_editors_flush_edits(const bContext *C, bool for_render)
{
	bool has_edited = false;
	Object *ob;
	Main *bmain = CTX_data_main(C);

	/* loop through all data to find edit mode or object mode, because during
	 * exiting we might not have a context for edit object and multiple sculpt
	 * objects can exist at the same time */
	for (ob = bmain->object.first; ob; ob = ob->id.next) {
		if (ob->mode & OB_MODE_SCULPT) {
			/* Don't allow flushing while in the middle of a stroke (frees data in use).
			 * Auto-save prevents this from happening but scripts may cause a flush on saving: T53986. */
			if ((ob->sculpt && ob->sculpt->cache) == 0) {
				/* flush multires changes (for sculpt) */
				multires_force_update(ob);
				has_edited = true;

				if (for_render) {
					/* flush changes from dynamic topology sculpt */
					BKE_sculptsession_bm_to_me_for_render(ob);
				}
				else {
					/* Set reorder=false so that saving the file doesn't reorder
					 * the BMesh's elements */
					BKE_sculptsession_bm_to_me(ob, false);
				}
			}
		}
		else if (ob->mode & OB_MODE_EDIT) {
			/* get editmode results */
			has_edited = true;
			ED_object_editmode_load(bmain, ob);
		}
	}

	return has_edited;
}

/* ***** XXX: functions are using old blender names, cleanup later ***** */


/* now only used in 2d spaces, like time, ipo, nla, sima... */
/* XXX shift/ctrl not configurable */
void apply_keyb_grid(int shift, int ctrl, float *val, float fac1, float fac2, float fac3, int invert)
{
	/* fac1 is for 'nothing', fac2 for CTRL, fac3 for SHIFT */
	if (invert)
		ctrl = !ctrl;

	if (ctrl && shift) {
		if (fac3 != 0.0f) *val = fac3 * floorf(*val / fac3 + 0.5f);
	}
	else if (ctrl) {
		if (fac2 != 0.0f) *val = fac2 * floorf(*val / fac2 + 0.5f);
	}
	else {
		if (fac1 != 0.0f) *val = fac1 * floorf(*val / fac1 + 0.5f);
	}
}

void unpack_menu(bContext *C, const char *opname, const char *id_name, const char *abs_name, const char *folder, struct PackedFile *pf)
{
	Main *bmain = CTX_data_main(C);
	PointerRNA props_ptr;
	uiPopupMenu *pup;
	uiLayout *layout;
	char line[FILE_MAX + 100];
	wmOperatorType *ot = WM_operatortype_find(opname, 1);

	pup = UI_popup_menu_begin(C, IFACE_("Unpack File"), ICON_NONE);
	layout = UI_popup_menu_layout(pup);

	uiItemFullO_ptr(
	        layout, ot, IFACE_("Remove Pack"), ICON_NONE,
	        NULL, WM_OP_EXEC_DEFAULT, 0, &props_ptr);
	RNA_enum_set(&props_ptr, "method", PF_REMOVE);
	RNA_string_set(&props_ptr, "id", id_name);

	if (G.relbase_valid) {
		char local_name[FILE_MAXDIR + FILE_MAX], fi[FILE_MAX];

		BLI_split_file_part(abs_name, fi, sizeof(fi));
		BLI_snprintf(local_name, sizeof(local_name), "//%s/%s", folder, fi);
		if (!STREQ(abs_name, local_name)) {
			switch (checkPackedFile(BKE_main_blendfile_path(bmain), local_name, pf)) {
				case PF_NOFILE:
					BLI_snprintf(line, sizeof(line), IFACE_("Create %s"), local_name);
					uiItemFullO_ptr(layout, ot, line, ICON_NONE, NULL, WM_OP_EXEC_DEFAULT, 0, &props_ptr);
					RNA_enum_set(&props_ptr, "method", PF_WRITE_LOCAL);
					RNA_string_set(&props_ptr, "id", id_name);

					break;
				case PF_EQUAL:
					BLI_snprintf(line, sizeof(line), IFACE_("Use %s (identical)"), local_name);
					//uiItemEnumO_ptr(layout, ot, line, 0, "method", PF_USE_LOCAL);
					uiItemFullO_ptr(layout, ot, line, ICON_NONE, NULL, WM_OP_EXEC_DEFAULT, 0, &props_ptr);
					RNA_enum_set(&props_ptr, "method", PF_USE_LOCAL);
					RNA_string_set(&props_ptr, "id", id_name);

					break;
				case PF_DIFFERS:
					BLI_snprintf(line, sizeof(line), IFACE_("Use %s (differs)"), local_name);
					//uiItemEnumO_ptr(layout, ot, line, 0, "method", PF_USE_LOCAL);
					uiItemFullO_ptr(layout, ot, line, ICON_NONE, NULL, WM_OP_EXEC_DEFAULT, 0, &props_ptr);
					RNA_enum_set(&props_ptr, "method", PF_USE_LOCAL);
					RNA_string_set(&props_ptr, "id", id_name);

					BLI_snprintf(line, sizeof(line), IFACE_("Overwrite %s"), local_name);
					//uiItemEnumO_ptr(layout, ot, line, 0, "method", PF_WRITE_LOCAL);
					uiItemFullO_ptr(layout, ot, line, ICON_NONE, NULL, WM_OP_EXEC_DEFAULT, 0, &props_ptr);
					RNA_enum_set(&props_ptr, "method", PF_WRITE_LOCAL);
					RNA_string_set(&props_ptr, "id", id_name);
					break;
			}
		}
	}

	switch (checkPackedFile(BKE_main_blendfile_path(bmain), abs_name, pf)) {
		case PF_NOFILE:
			BLI_snprintf(line, sizeof(line), IFACE_("Create %s"), abs_name);
			//uiItemEnumO_ptr(layout, ot, line, 0, "method", PF_WRITE_ORIGINAL);
			uiItemFullO_ptr(layout, ot, line, ICON_NONE, NULL, WM_OP_EXEC_DEFAULT, 0, &props_ptr);
			RNA_enum_set(&props_ptr, "method", PF_WRITE_ORIGINAL);
			RNA_string_set(&props_ptr, "id", id_name);
			break;
		case PF_EQUAL:
			BLI_snprintf(line, sizeof(line), IFACE_("Use %s (identical)"), abs_name);
			//uiItemEnumO_ptr(layout, ot, line, 0, "method", PF_USE_ORIGINAL);
			uiItemFullO_ptr(layout, ot, line, ICON_NONE, NULL, WM_OP_EXEC_DEFAULT, 0, &props_ptr);
			RNA_enum_set(&props_ptr, "method", PF_USE_ORIGINAL);
			RNA_string_set(&props_ptr, "id", id_name);
			break;
		case PF_DIFFERS:
			BLI_snprintf(line, sizeof(line), IFACE_("Use %s (differs)"), abs_name);
			//uiItemEnumO_ptr(layout, ot, line, 0, "method", PF_USE_ORIGINAL);
			uiItemFullO_ptr(layout, ot, line, ICON_NONE, NULL, WM_OP_EXEC_DEFAULT, 0, &props_ptr);
			RNA_enum_set(&props_ptr, "method", PF_USE_ORIGINAL);
			RNA_string_set(&props_ptr, "id", id_name);

			BLI_snprintf(line, sizeof(line), IFACE_("Overwrite %s"), abs_name);
			//uiItemEnumO_ptr(layout, ot, line, 0, "method", PF_WRITE_ORIGINAL);
			uiItemFullO_ptr(layout, ot, line, ICON_NONE, NULL, WM_OP_EXEC_DEFAULT, 0, &props_ptr);
			RNA_enum_set(&props_ptr, "method", PF_WRITE_ORIGINAL);
			RNA_string_set(&props_ptr, "id", id_name);
			break;
	}

	UI_popup_menu_end(C, pup);
}

/* ********************* generic callbacks for drawcall api *********************** */

/**
 * Callback that draws a line between the mouse and a position given as the initial argument.
 */
void ED_region_draw_mouse_line_cb(const bContext *C, ARegion *ar, void *arg_info)
{
	wmWindow *win = CTX_wm_window(C);
	const float *mval_src = (float *)arg_info;
	const float mval_dst[2] = {win->eventstate->x - ar->winrct.xmin,
	                           win->eventstate->y - ar->winrct.ymin};

	const uint shdr_pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);

	GPU_line_width(1.0f * U.pixelsize);

	immBindBuiltinProgram(GPU_SHADER_2D_LINE_DASHED_UNIFORM_COLOR);

	float viewport_size[4];
	GPU_viewport_size_get_f(viewport_size);
	immUniform2f("viewport_size", viewport_size[2] / UI_DPI_FAC, viewport_size[3] / UI_DPI_FAC);

	immUniform1i("colors_len", 0);  /* "simple" mode */
	immUniformThemeColor(TH_VIEW_OVERLAY);
	immUniform1f("dash_width", 6.0f);
	immUniform1f("dash_factor", 0.5f);

	immBegin(GPU_PRIM_LINES, 2);
	immVertex2fv(shdr_pos, mval_src);
	immVertex2fv(shdr_pos, mval_dst);
	immEnd();

	immUnbindProgram();
}

/**
 * Use to free ID references within runtime data (stored outside of DNA)
 *
 * \param new_id may be NULL to unlink \a old_id.
 */
void ED_spacedata_id_remap(struct ScrArea *sa, struct SpaceLink *sl, ID *old_id, ID *new_id)
{
	SpaceType *st = BKE_spacetype_from_id(sl->spacetype);

	if (st && st->id_remap) {
		st->id_remap(sa, sl, old_id, new_id);
	}
}

static int ed_flush_edits_exec(bContext *C, wmOperator *UNUSED(op))
{
	ED_editors_flush_edits(C, false);
	return OPERATOR_FINISHED;
}

void ED_OT_flush_edits(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Flush Edits";
	ot->description = "Flush edit data from active editing modes";
	ot->idname = "ED_OT_flush_edits";

	/* api callbacks */
	ot->exec = ed_flush_edits_exec;

	/* flags */
	ot->flag = OPTYPE_INTERNAL;
}