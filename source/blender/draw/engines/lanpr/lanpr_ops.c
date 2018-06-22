#include "DRW_engine.h"
#include "DRW_render.h"
#include "BLI_listbase.h"
#include "BLI_linklist.h"
#include "lanpr_all.h"
#include "lanpr_util.h"
#include "DRW_render.h"
#include "BKE_object.h"
#include "DNA_mesh_types.h"
#include "DNA_camera_types.h"
#include "GPU_immediate.h"
#include "GPU_immediate_util.h"
#include "GPU_framebuffer.h"
#include "DNA_lanpr_types.h"
#include "DNA_meshdata_types.h"
#include "BKE_customdata.h"
#include "DEG_depsgraph_query.h"
#include "GPU_draw.h"

#include "GPU_batch.h"
#include "GPU_framebuffer.h"
#include "GPU_shader.h"
#include "GPU_uniformbuffer.h"
#include "GPU_viewport.h"
#include "bmesh.h"

#include "WM_types.h"
#include "WM_api.h"

#include <math.h>

/*

Ported from NUL4.0

Author(s):WuYiming - xp8110@outlook.com

*/

struct Object;


int lanpr_TriangleLineImageSpaceIntersectTestOnlyV2(LANPR_RenderTriangle* rt, LANPR_RenderLine* rl, Camera* cam, tnsMatrix44d vp, double* From, double* To);


/* ====================================== base structures =========================================== */

#define TNS_BOUND_AREA_CROSSES(b1,b2)\
((b1)[0]<(b2)[1] && (b1)[1]>(b2)[0] && (b1)[3]<(b2)[2] && (b1)[2]>(b2)[3])

void lanpr_MakeInitialBoundingAreas(LANPR_RenderBuffer* rb) {
	int SpW = 20;
	int SpH = rb->H / (rb->W / SpW);
	int Row, Col;
	LANPR_BoundingArea* ba;
	real W = (real)rb->W;
	real H = (real)rb->H;
	real SpanW = (real)1 / SpW * 2.0;
	real SpanH = (real)1 / SpH * 2.0;

	rb->TileCountX = SpW;
	rb->TileCountY = SpH;
	rb->WidthPerTile = SpanW;
	rb->HeightPerTile = SpanH;

	rb->BoundingAreaCount = SpW * SpH;
	rb->InitialBoundingAreas = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_BoundingArea) * rb->BoundingAreaCount);

	for (Row = 0; Row < SpH; Row++) {
		for (Col = 0; Col < SpW; Col++) {
			ba = &rb->InitialBoundingAreas[Row * 20 + Col];

			ba->L = SpanW * Col - 1.0;
			ba->R = (Col == SpW - 1) ? 1.0 : (SpanW * (Col + 1) - 1.0);
			ba->U = 1.0 - SpanH * Row;
			ba->B = (Row == SpH - 1) ? -1.0 : (1.0 - SpanH * (Row + 1));

			ba->CX = (ba->L + ba->R) / 2;
			ba->CY = (ba->U + ba->B) / 2;

			if (Row) {
				lstAppendPointerStatic(&ba->UP, &rb->RenderDataPool, &rb->InitialBoundingAreas[(Row - 1) * 20 + Col]);
			}
			if (Col) {
				lstAppendPointerStatic(&ba->LP, &rb->RenderDataPool, &rb->InitialBoundingAreas[Row * 20 + Col - 1]);
			}
			if (Row != SpH -1) {
				lstAppendPointerStatic(&ba->BP, &rb->RenderDataPool, &rb->InitialBoundingAreas[(Row + 1) * 20 + Col]);
			}
			if (Col != SpW - 1) {
				lstAppendPointerStatic(&ba->RP, &rb->RenderDataPool, &rb->InitialBoundingAreas[Row * 20 + Col + 1]);
			}
		}
	}
}
void lanpr_ConnectNewBoundingAreas(LANPR_RenderBuffer* rb, LANPR_BoundingArea* Root) {
	LANPR_BoundingArea* ba = Root->Child, *tba;
	nListItemPointer* lip,*lip2,*NextLip;
	nStaticMemoryPool* mph= &rb->RenderDataPool;

	lstAppendPointerStaticPool(mph, &ba[1].RP, &ba[0]);
	lstAppendPointerStaticPool(mph, &ba[0].LP, &ba[1]);
	lstAppendPointerStaticPool(mph, &ba[1].BP, &ba[2]);
	lstAppendPointerStaticPool(mph, &ba[2].UP, &ba[1]);
	lstAppendPointerStaticPool(mph, &ba[2].RP, &ba[3]);
	lstAppendPointerStaticPool(mph, &ba[3].LP, &ba[2]);
	lstAppendPointerStaticPool(mph, &ba[3].UP, &ba[0]);
	lstAppendPointerStaticPool(mph, &ba[0].BP, &ba[3]);

	for (lip = Root->LP.pFirst; lip; lip = lip->pNext) {
		tba = lip->p;
		if (ba[1].U > tba->B && ba[1].B < tba->U) { lstAppendPointerStaticPool(mph, &ba[1].LP, tba); lstAppendPointerStaticPool(mph, &tba->RP, &ba[1]); }
		if (ba[2].U > tba->B && ba[2].B < tba->U) { lstAppendPointerStaticPool(mph, &ba[2].LP, tba); lstAppendPointerStaticPool(mph, &tba->RP, &ba[2]); }
	}
	for (lip = Root->RP.pFirst; lip; lip = lip->pNext) {
		tba = lip->p;
		if (ba[0].U > tba->B && ba[0].B < tba->U) { lstAppendPointerStaticPool(mph, &ba[0].RP, tba); lstAppendPointerStaticPool(mph, &tba->LP, &ba[0]); }
		if (ba[3].U > tba->B && ba[3].B < tba->U) { lstAppendPointerStaticPool(mph, &ba[3].RP, tba); lstAppendPointerStaticPool(mph, &tba->LP, &ba[3]); }
	}
	for (lip = Root->UP.pFirst; lip; lip = lip->pNext) {
		tba = lip->p;
		if (ba[0].R > tba->L && ba[0].L < tba->R) { lstAppendPointerStaticPool(mph, &ba[0].UP, tba); lstAppendPointerStaticPool(mph, &tba->BP, &ba[0]); }
		if (ba[1].R > tba->L && ba[1].L < tba->R) { lstAppendPointerStaticPool(mph, &ba[1].UP, tba); lstAppendPointerStaticPool(mph, &tba->BP, &ba[1]); }
	}
	for (lip = Root->BP.pFirst; lip; lip = lip->pNext) {
		tba = lip->p;
		if (ba[2].R > tba->L && ba[2].L < tba->R) { lstAppendPointerStaticPool(mph, &ba[2].BP, tba); lstAppendPointerStaticPool(mph, &tba->UP, &ba[2]); }
		if (ba[3].R > tba->L && ba[3].L < tba->R) { lstAppendPointerStaticPool(mph, &ba[3].BP, tba); lstAppendPointerStaticPool(mph, &tba->UP, &ba[3]); }
	}
	for (lip = Root->LP.pFirst; lip; lip = lip->pNext) {
		for (lip2 = ((LANPR_BoundingArea*)lip->p)->RP.pFirst; lip2; lip2 = NextLip) {
			NextLip = lip2->pNext;
			tba = lip2->p;
			if (tba == Root) {
				lstRemovePointerItemNoFree(&((LANPR_BoundingArea*)lip->p)->RP, lip2);
				if (ba[1].U > tba->B && ba[1].B < tba->U)lstAppendPointerStaticPool(mph, &tba->RP, &ba[1]);
				if (ba[2].U > tba->B && ba[2].B < tba->U)lstAppendPointerStaticPool(mph, &tba->RP, &ba[2]);
			}
		}
	}
	for (lip = Root->RP.pFirst; lip; lip = lip->pNext) {
		for (lip2 = ((LANPR_BoundingArea*)lip->p)->LP.pFirst; lip2; lip2 = NextLip) {
			NextLip = lip2->pNext;
			tba = lip2->p;
			if (tba == Root) {
				lstRemovePointerItemNoFree(&((LANPR_BoundingArea*)lip->p)->LP, lip2);
				if (ba[0].U > tba->B && ba[0].B < tba->U)lstAppendPointerStaticPool(mph, &tba->LP, &ba[0]);
				if (ba[3].U > tba->B && ba[3].B < tba->U)lstAppendPointerStaticPool(mph, &tba->LP, &ba[3]);
			}
		}
	}
	for (lip = Root->UP.pFirst; lip; lip = lip->pNext) {
		for (lip2 = ((LANPR_BoundingArea*)lip->p)->BP.pFirst; lip2; lip2 = NextLip) {
			NextLip = lip2->pNext;
			tba = lip2->p;
			if (tba == Root) {
				lstRemovePointerItemNoFree(&((LANPR_BoundingArea*)lip->p)->BP, lip2);
				if (ba[0].R > tba->L && ba[0].L < tba->R)lstAppendPointerStaticPool(mph, &tba->UP, &ba[0]);
				if (ba[1].R > tba->L && ba[1].L < tba->R)lstAppendPointerStaticPool(mph, &tba->UP, &ba[1]);
			}
		}
	}
	for (lip = Root->BP.pFirst; lip; lip = lip->pNext) {
		for (lip2 = ((LANPR_BoundingArea*)lip->p)->UP.pFirst; lip2; lip2 = NextLip) {
			NextLip = lip2->pNext;
			tba = lip2->p;
			if (tba == Root) {
				lstRemovePointerItemNoFree(&((LANPR_BoundingArea*)lip->p)->UP, lip2);
				if (ba[2].R > tba->L && ba[2].L < tba->R)lstAppendPointerStaticPool(mph, &tba->BP, &ba[2]);
				if (ba[3].R > tba->L && ba[3].L < tba->R)lstAppendPointerStaticPool(mph, &tba->BP, &ba[3]);
			}
		}
	}
	while (lstPopPointerNoFree(&Root->LP));
	while (lstPopPointerNoFree(&Root->RP));
	while (lstPopPointerNoFree(&Root->UP));
	while (lstPopPointerNoFree(&Root->BP));
}
void lanpr_AssociateTriangleWithBoundingArea(LANPR_RenderBuffer* rb, LANPR_BoundingArea* RootBoundingArea, LANPR_RenderTriangle* rt, real* LRUB, int Recursive);
int lanpr_TriangleCalculateIntersectionsInTile(LANPR_RenderBuffer* rb, LANPR_RenderTriangle* rt, LANPR_BoundingArea* ba);

void lanpr_SplitBoundingArea(LANPR_RenderBuffer* rb, LANPR_BoundingArea* Root) {
	LANPR_BoundingArea* ba = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_BoundingArea) * 4);
	LANPR_RenderTriangle* rt;

	ba[0].L = Root->CX;
	ba[0].R = Root->R;
	ba[0].U = Root->U;
	ba[0].B = Root->CY;
	ba[0].CX = (ba[0].L + ba[0].R) / 2;
	ba[0].CY = (ba[0].U + ba[0].B) / 2;

	ba[1].L = Root->L;
	ba[1].R = Root->CX;
	ba[1].U = Root->U;
	ba[1].B = Root->CY;
	ba[1].CX = (ba[1].L + ba[1].R) / 2;
	ba[1].CY = (ba[1].U + ba[1].B) / 2;

	ba[2].L = Root->L;
	ba[2].R = Root->CX;
	ba[2].U = Root->CY;
	ba[2].B = Root->B;
	ba[2].CX = (ba[2].L + ba[2].R) / 2;
	ba[2].CY = (ba[2].U + ba[2].B) / 2;

	ba[3].L = Root->CX;
	ba[3].R = Root->R;
	ba[3].U = Root->CY;
	ba[3].B = Root->B;
	ba[3].CX = (ba[3].L + ba[3].R) / 2;
	ba[3].CY = (ba[3].U + ba[3].B) / 2;

	Root->Child = ba;

	lanpr_ConnectNewBoundingAreas(rb, Root);

	while (rt = lstPopPointerNoFree(&Root->AssociatedTriangles)) {
		LANPR_BoundingArea* ba = Root->Child;
		real B[4];
		B[0] = TNS_MIN3(rt->V[0]->FrameBufferCoord[0], rt->V[1]->FrameBufferCoord[0], rt->V[2]->FrameBufferCoord[0]);
		B[1] = TNS_MAX3(rt->V[0]->FrameBufferCoord[0], rt->V[1]->FrameBufferCoord[0], rt->V[2]->FrameBufferCoord[0]);
		B[2] = TNS_MAX3(rt->V[0]->FrameBufferCoord[1], rt->V[1]->FrameBufferCoord[1], rt->V[2]->FrameBufferCoord[1]);
		B[3] = TNS_MIN3(rt->V[0]->FrameBufferCoord[1], rt->V[1]->FrameBufferCoord[1], rt->V[2]->FrameBufferCoord[1]);
		if (TNS_BOUND_AREA_CROSSES(B, &ba[0].L)) lanpr_AssociateTriangleWithBoundingArea(rb, &ba[0], rt, B, 0);
		if (TNS_BOUND_AREA_CROSSES(B, &ba[1].L)) lanpr_AssociateTriangleWithBoundingArea(rb, &ba[1], rt, B, 0);
		if (TNS_BOUND_AREA_CROSSES(B, &ba[2].L)) lanpr_AssociateTriangleWithBoundingArea(rb, &ba[2], rt, B, 0);
		if (TNS_BOUND_AREA_CROSSES(B, &ba[3].L)) lanpr_AssociateTriangleWithBoundingArea(rb, &ba[3], rt, B, 0);
	}

	rb->BoundingAreaCount += 3;
}
int lanpr_LineCrossesBoundingArea(LANPR_RenderBuffer* fb, tnsVector2d L, tnsVector2d R, LANPR_BoundingArea* ba) {
	real vx, vy;
	tnsVector4d Converted;
	real c1, c;

	if ((Converted[0] = (real)ba->L) > TNS_MAX2(L[0], R[0])) return 0;
	if ((Converted[1] = (real)ba->R) < TNS_MIN2(L[0], R[0])) return 0;
	if ((Converted[2] = (real)ba->B) > TNS_MAX2(L[1], R[1])) return 0;
	if ((Converted[3] = (real)ba->U) < TNS_MIN2(L[1], R[1])) return 0;

	vx = L[0] - R[0];
	vy = L[1] - R[1];

	c1 = vx * (Converted[2] - L[1]) - vy * (Converted[0] - L[0]);
	c = c1;

	c1 = vx * (Converted[2] - L[1]) - vy * (Converted[1] - L[0]);
	if (c1*c <= 0)return 1;
	else c = c1;

	c1 = vx * (Converted[3] - L[1]) - vy * (Converted[0] - L[0]);
	if (c1*c <= 0)return 1;
	else c = c1;

	c1 = vx * (Converted[3] - L[1]) - vy * (Converted[1] - L[0]);
	if (c1*c <= 0)return 1;
	else c = c1;

	return 0;
}
int lanpr_TriangleCoversBoundingArea(LANPR_RenderBuffer* fb, LANPR_RenderTriangle* rt, LANPR_BoundingArea* ba) {
	tnsVector2d p1, p2, p3, p4;
	real
		*FBC1 = rt->V[0]->FrameBufferCoord,
		*FBC2 = rt->V[1]->FrameBufferCoord,
		*FBC3 = rt->V[2]->FrameBufferCoord;

	p3[0] = p1[0] = (real)ba->L;
	p2[1] = p1[1] = (real)ba->B;
	p2[0] = p4[0] = (real)ba->R;
	p3[1] = p4[1] = (real)ba->U;

	if (FBC1[0] >= p1[0] && FBC1[0] <= p2[0] && FBC1[1] >= p1[1] && FBC1[1] <= p3[1]) return 1;
	if (FBC2[0] >= p1[0] && FBC2[0] <= p2[0] && FBC2[1] >= p1[1] && FBC2[1] <= p3[1]) return 1;
	if (FBC3[0] >= p1[0] && FBC3[0] <= p2[0] && FBC3[1] >= p1[1] && FBC3[1] <= p3[1]) return 1;

	if (lanpr_PointInsideTrianglef(p1, FBC1, FBC2, FBC3) ||
		lanpr_PointInsideTrianglef(p2, FBC1, FBC2, FBC3) ||
		lanpr_PointInsideTrianglef(p3, FBC1, FBC2, FBC3) ||
		lanpr_PointInsideTrianglef(p4, FBC1, FBC2, FBC3)) return 1;

	if  (lanpr_LineCrossesBoundingArea(fb, FBC1, FBC2, ba)) return 1;
	elif(lanpr_LineCrossesBoundingArea(fb, FBC2, FBC3, ba)) return 1;
	elif(lanpr_LineCrossesBoundingArea(fb, FBC3, FBC1, ba)) return 1;

	return 0;
}
void lanpr_AssociateTriangleWithBoundingArea(LANPR_RenderBuffer* rb, LANPR_BoundingArea* RootBoundingArea, LANPR_RenderTriangle* rt, real* LRUB, int Recursive) {
	if (!lanpr_TriangleCoversBoundingArea(rb, rt, RootBoundingArea)) return;
	if (!RootBoundingArea->Child) {
		lstAppendPointerStaticPool(&rb->RenderDataPool, &RootBoundingArea->AssociatedTriangles, rt);
		RootBoundingArea->TriangleCount++;
		if (RootBoundingArea->TriangleCount > 200 && Recursive) {
			lanpr_SplitBoundingArea(rb, RootBoundingArea);
		}
		if(Recursive) lanpr_TriangleCalculateIntersectionsInTile(rb, rt, RootBoundingArea);
	}else {
		LANPR_BoundingArea* ba = RootBoundingArea->Child;
		real* B1 = LRUB;
		real B[4];
		if (!LRUB) {
			B[0] = TNS_MIN3(rt->V[0]->FrameBufferCoord[0], rt->V[1]->FrameBufferCoord[0], rt->V[2]->FrameBufferCoord[0]);
			B[1] = TNS_MAX3(rt->V[0]->FrameBufferCoord[0], rt->V[1]->FrameBufferCoord[0], rt->V[2]->FrameBufferCoord[0]);
			B[2] = TNS_MAX3(rt->V[0]->FrameBufferCoord[1], rt->V[1]->FrameBufferCoord[1], rt->V[2]->FrameBufferCoord[1]);
			B[3] = TNS_MIN3(rt->V[0]->FrameBufferCoord[1], rt->V[1]->FrameBufferCoord[1], rt->V[2]->FrameBufferCoord[1]);
			B1 = B;
		}
		if (TNS_BOUND_AREA_CROSSES(B1, &ba[0].L)) lanpr_AssociateTriangleWithBoundingArea(rb, &ba[0], rt, B1, Recursive);
		if (TNS_BOUND_AREA_CROSSES(B1, &ba[1].L)) lanpr_AssociateTriangleWithBoundingArea(rb, &ba[1], rt, B1, Recursive);
		if (TNS_BOUND_AREA_CROSSES(B1, &ba[2].L)) lanpr_AssociateTriangleWithBoundingArea(rb, &ba[2], rt, B1, Recursive);
		if (TNS_BOUND_AREA_CROSSES(B1, &ba[3].L)) lanpr_AssociateTriangleWithBoundingArea(rb, &ba[3], rt, B1, Recursive);
	}
}
int lanpr_GetTriangleBoundingTile(LANPR_RenderBuffer* rb, LANPR_RenderTriangle* rt,int* RowBegin,int* RowEnd,int* ColBegin,int* ColEnd) {
	real SpW = rb->WidthPerTile, SpH = rb->HeightPerTile;
	real B[4];

	if (!rt->F || !rt->V[0] || !rt->V[1] || !rt->V[2]) return 0;

	B[0] = TNS_MIN3(rt->V[0]->FrameBufferCoord[0], rt->V[1]->FrameBufferCoord[0], rt->V[2]->FrameBufferCoord[0]);
	B[1] = TNS_MAX3(rt->V[0]->FrameBufferCoord[0], rt->V[1]->FrameBufferCoord[0], rt->V[2]->FrameBufferCoord[0]);
	B[2] = TNS_MIN3(rt->V[0]->FrameBufferCoord[1], rt->V[1]->FrameBufferCoord[1], rt->V[2]->FrameBufferCoord[1]);
	B[3] = TNS_MAX3(rt->V[0]->FrameBufferCoord[1], rt->V[1]->FrameBufferCoord[1], rt->V[2]->FrameBufferCoord[1]);

	if (B[0] > 1 || B[1] < -1 || B[2]>1 || B[3] < -1) return 0;

	(*ColBegin) = (int)((B[0] + 1.0) / SpW);
	(*ColEnd) = (int)((B[1] + 1.0) / SpW);
	(*RowEnd) = rb->TileCountY - (int)((B[2] + 1.0) / SpH) - 1;
	(*RowBegin) = rb->TileCountY - (int)((B[3] + 1.0) / SpH) - 1;

	if ((*ColEnd) >= rb->TileCountX) (*ColEnd) = rb->TileCountX-1;
	if ((*RowEnd) >= rb->TileCountY) (*RowEnd) = rb->TileCountY-1;
	if ((*ColBegin) < 0) (*ColBegin) = 0;
	if ((*RowBegin) < 0) (*RowBegin) = 0;

	return 1;
}
void lanpr_AddTriangles(LANPR_RenderBuffer* rb) {
	LANPR_RenderElementLinkNode* reln;
	LANPR_RenderTriangle* rt;
	tnsMatrix44d VP;
	Camera* c = ((Camera*)rb->Scene->camera);
	int i, lim;
	int x1, x2, y1, y2;
	int r, co;
	//tnsMatrix44d proj, view, result, inv;
	//tMatMakePerspectiveMatrix44d(proj, c->FOV, (real)fb->W / (real)fb->H, c->clipsta, c->clipend);
	//tMatLoadIdentity44d(view);
	//tObjApplySelfTransformMatrix(c, 0);
	//tObjApplyGlobalTransformMatrixReverted(c);
	//tMatInverse44d(inv, c->Base.GlobalTransform);
	//tMatMultiply44d(result, proj, inv);
	//memcpy(proj, result, sizeof(tnsMatrix44d));

	//tnsglobal_TriangleIntersectionCount = 0;

	//tnsset_RenderOverallProgress(rb, NUL_MH2);
	rb->CalculationStatus = TNS_CALCULATION_INTERSECTION;
	//nulThreadNotifyUsers("tns.render_buffer_list.calculation_status");

	for (reln = rb->TriangleBufferPointers.pFirst; reln; reln = reln->Item.pNext) {
		rt = reln->Pointer;
		lim = reln->ElementCount;
		for (i = 0; i < lim; i++) {
			if (rt->CullStatus) {
				rt++; continue;
			}
			if (lanpr_GetTriangleBoundingTile(rb, rt, &y1, &y2, &x1, &x2)) {
				for (co = x1; co <= x2; co++) {
					for (r = y1; r <= y2; r++) {
						lanpr_AssociateTriangleWithBoundingArea(rb, &rb->InitialBoundingAreas[r * 20 + co], rt, 0, 1);
					}
				}
			}else{
				;//throw away.
			}
			rt = (void*)(((BYTE*)rt) + rb->TriangleSize);
			//if (tnsglobal_TriangleIntersectionCount >= 2000) {
				//tnsset_PlusRenderIntersectionCount(rb, tnsglobal_TriangleIntersectionCount);
				//tnsglobal_TriangleIntersectionCount = 0;
			//}
		}
	}
	//tnsset_PlusRenderIntersectionCount(rb, tnsglobal_TriangleIntersectionCount);
}
LANPR_BoundingArea* lanpr_GetNextBoundingArea(LANPR_BoundingArea* This, LANPR_RenderLine* rl, real x, real y, real k, int PositiveX, int PositiveY, real* NextX, real* NextY) {
	real rx, ry, ux, uy, lx, ly, bx, by;
	real r1, r2, r;
	LANPR_BoundingArea* ba; nListItemPointer* lip;
	if (PositiveX) {
		rx = This->R;
		ry = y + k * (rx - x);
		if (PositiveY) {
			uy = This->U;
			ux = x + (uy - y) / k;
			r1 = tMatGetLinearRatio(rl->L->FrameBufferCoord[0], rl->R->FrameBufferCoord[0], rx);
			r2 = tMatGetLinearRatio(rl->L->FrameBufferCoord[0], rl->R->FrameBufferCoord[0], ux);
			if (TNS_MIN2(r1, r2) > 1) return 0;
			if (r1 <= r2) {
				for (lip = This->RP.pFirst; lip; lip = lip->pNext) {
					ba = lip->p;
					if (ba->U >= ry && ba->B < ry) { *NextX = rx; *NextY = ry; return ba; }
				}
			}else {
				for (lip = This->UP.pFirst; lip; lip = lip->pNext) {
					ba = lip->p;
					if (ba->R >= ux && ba->L < ux) { *NextX = ux; *NextY = uy; return ba; }
				}
			}
		}else {
			by = This->B;
			bx = x + (by - y) / k;
			r1 = tMatGetLinearRatio(rl->L->FrameBufferCoord[0], rl->R->FrameBufferCoord[0], rx);
			r2 = tMatGetLinearRatio(rl->L->FrameBufferCoord[0], rl->R->FrameBufferCoord[0], bx);
			if (TNS_MIN2(r1, r2) > 1) return 0;
			if (r1 <= r2) {
				for (lip = This->RP.pFirst; lip; lip = lip->pNext) {
					ba = lip->p;
					if (ba->U >= ry && ba->B < ry) { *NextX = rx; *NextY = ry; return ba; }
				}
			}else {
				for (lip = This->BP.pFirst; lip; lip = lip->pNext) {
					ba = lip->p;
					if (ba->R >= bx && ba->L < bx) { *NextX = bx; *NextY = by; return ba; }
				}
			}
		}
	}else {
		lx = This->L;
		ly = y + k * (lx - x);
		if (PositiveY) {
			uy = This->U;
			ux = x + (uy - y) / k;
			r1 = tMatGetLinearRatio(rl->L->FrameBufferCoord[0], rl->R->FrameBufferCoord[0], lx);
			r2 = tMatGetLinearRatio(rl->L->FrameBufferCoord[0], rl->R->FrameBufferCoord[0], ux);
			if (TNS_MIN2(r1, r2) > 1) return 0;
			if (r1 <= r2) {
				for (lip = This->LP.pFirst; lip; lip = lip->pNext) {
					ba = lip->p;
					if (ba->U >= ly && ba->B < ly) { *NextX = lx; *NextY = ly; return ba; }
				}
			}else {
				for (lip = This->UP.pFirst; lip; lip = lip->pNext) {
					ba = lip->p;
					if (ba->R >= ux && ba->L < ux) { *NextX = ux; *NextY = uy; return ba; }
				}
			}
		}else {
			by = This->B;
			bx = x + (by - y) / k;
			r1 = tMatGetLinearRatio(rl->L->FrameBufferCoord[0], rl->R->FrameBufferCoord[0], lx);
			r2 = tMatGetLinearRatio(rl->L->FrameBufferCoord[0], rl->R->FrameBufferCoord[0], bx);
			if (TNS_MIN2(r1, r2) > 1) return 0;
			if (r1 <= r2) {
				for (lip = This->LP.pFirst; lip; lip = lip->pNext) {
					ba = lip->p;
					if (ba->U >= ly && ba->B < ly) { *NextX = lx; *NextY = ly; return ba; }
				}
			}else {
				for (lip = This->BP.pFirst; lip; lip = lip->pNext) {
					ba = lip->p;
					if (ba->R >= bx && ba->L < bx) { *NextX = bx; *NextY = by; return ba; }
				}
			}
		}
	}
	return 0;
}

LANPR_BoundingArea* lanpr_GetBoundingArea(LANPR_RenderBuffer* rb, real x, real y) {
	LANPR_BoundingArea* iba;
	real SpW = rb->WidthPerTile, SpH = rb->HeightPerTile;
	int c = (int)((x + 1.0) / SpW);
	int r = rb->TileCountY - (int)((y + 1.0) / SpH) - 1;
	if (r < 0) r = 0;
	if (c < 0) c = 0;
	if (r >= rb->TileCountY) r = rb->TileCountY-1;
	if (c >= rb->TileCountX) c = rb->TileCountX-1;

	iba = &rb->InitialBoundingAreas[r * 20 + c];
	while (iba->Child) {
		if (x > iba->CX) {
			if (y > iba->CY) iba = &iba->Child[0];
			else iba = &iba->Child[3];
		}else {
			if (y > iba->CY) iba = &iba->Child[1];
			else iba = &iba->Child[2];
		}
	}
	return iba;
}
LANPR_BoundingArea* lanpr_GetFirstPossibleBoundingArea(LANPR_RenderBuffer* rb, LANPR_RenderLine* rl) {
	LANPR_BoundingArea* iba;
	real p[2] = { rl->L->FrameBufferCoord[0], rl->L->FrameBufferCoord[1] };
	tnsVector2d LU = { -1,1 }, RU = { 1,1 }, LB = { -1,-1 }, RB = { 1, -1 };
	real r=1,sr=1;

	if (p[0] > -1 && p[0]<1 && p[1] > -1 && p[1]<1) {
		return lanpr_GetBoundingArea(rb, p[0], p[1]);
	}else {
		if (lanpr_LineIntersectTest2d(rl->L->FrameBufferCoord, rl->R->FrameBufferCoord, LU, RU, &sr) && sr<r && sr>0)r = sr;
		if (lanpr_LineIntersectTest2d(rl->L->FrameBufferCoord, rl->R->FrameBufferCoord, LB, RB, &sr) && sr<r && sr>0)r = sr;
		if (lanpr_LineIntersectTest2d(rl->L->FrameBufferCoord, rl->R->FrameBufferCoord, LB, LU, &sr) && sr<r && sr>0)r = sr;
		if (lanpr_LineIntersectTest2d(rl->L->FrameBufferCoord, rl->R->FrameBufferCoord, RB, RU, &sr) && sr<r && sr>0)r = sr;
		lanpr_LinearInterpolate2dv(rl->L->FrameBufferCoord, rl->R->FrameBufferCoord, r, p);

		return lanpr_GetBoundingArea(rb, p[0], p[1]);
	}

	real SpW = rb->WidthPerTile, SpH = rb->HeightPerTile;

	return iba;
}


/* ======================================= geometry ============================================ */

void lanpr_CutLineIntegrated(LANPR_RenderBuffer* rb, LANPR_RenderLine* rl, real Begin, real End) {
	LANPR_RenderLineSegment* rls = rl->Segments.pFirst, *irls;
	LANPR_RenderLineSegment *BeginSegment = 0, *EndSegment = 0;
	LANPR_RenderLineSegment *ns = 0, *ns2 = 0;
	LANPR_RenderLineSegment *BeforeBegin, *AfterEnd;
	LANPR_RenderLineSegment *Next;

	if (TNS_DOUBLE_CLOSE_ENOUGH(Begin, End)) return;

	if (Begin != Begin)
		Begin = 0;
	if (End != End)
		End = 0;

	if (Begin > End) {
		real t = Begin;
		Begin = End;
		End = t;
	}

	for (rls = rl->Segments.pFirst; rls; rls = rls->Item.pNext) {
		if (TNS_DOUBLE_CLOSE_ENOUGH(rls->at, Begin)) {
			BeginSegment = rls;
			ns = BeginSegment;
			break;
		}
		if (!rls->Item.pNext) {
			break;
		}
		irls = rls->Item.pNext;
		if (irls->at > Begin && Begin > rls->at) {
			BeginSegment = irls;
			ns = memStaticAquireThread(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
			break;
		}
	}
	for (rls = BeginSegment; rls; rls = rls->Item.pNext) {
		if (TNS_DOUBLE_CLOSE_ENOUGH(rls->at, End)) {
			EndSegment = rls;
			ns2 = EndSegment;
			break;
		}
		//irls = rls->Item.pNext;
		if (rls->at > End) {
			EndSegment = rls;
			ns2 = memStaticAquireThread(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
			break;
		}
	}

	if (!ns) ns = memStaticAquireThread(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
	if (!ns2) ns2 = memStaticAquireThread(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));

	if (BeginSegment) {
		if (BeginSegment != ns) {
			ns->OccludeLevel = BeginSegment->Item.pPrev ? (irls = BeginSegment->Item.pPrev)->OccludeLevel : 0;
			lstInsertItemBefore(&rl->Segments, (void*)ns, (void*)BeginSegment);
		}
	}
	else {
		ns->OccludeLevel = (irls = rl->Segments.pLast)->OccludeLevel;
		lstAppendItem(&rl->Segments, ns);
	}
	if (EndSegment) {
		if (EndSegment != ns2) {
			ns2->OccludeLevel = EndSegment->Item.pPrev ? (irls = EndSegment->Item.pPrev)->OccludeLevel : 0;
			lstInsertItemBefore(&rl->Segments, (void*)ns2, (void*)EndSegment);
		}
	}
	else {
		ns2->OccludeLevel = (irls = rl->Segments.pLast)->OccludeLevel;
		lstAppendItem(&rl->Segments, ns2);
	}

	ns->at = Begin;
	ns2->at = End;

	for (rls = ns; rls && rls != ns2; rls = rls->Item.pNext) {
		rls->OccludeLevel++;
	}
}


int lanpr_MakeNextOcclusionTaskInfo(LANPR_RenderBuffer* rb, LANPR_RenderTaskInfo* rti) {
	nListItemPointer* p;
	int i;
	int res = 0;

	BLI_spin_lock(&rb->csManagement);
	
	if (rb->ContourManaged) {
		p = rb->ContourManaged;
		rti->Contour = (void*)p;
		rti->ContourPointers.pFirst = p;
		for (i = 0; i < TNS_THREAD_LINE_COUNT && p; i++) {
			p = p->pNext;
		}
		rb->ContourManaged = p;
		rti->ContourPointers.pLast = p ? p->pPrev : rb->Contours.pLast;
		res = 1;
	}
	else {
		lstEmptyDirect(&rti->ContourPointers);
		rti->Contour = 0;
	}

	if (rb->IntersectionManaged) {
		p = rb->IntersectionManaged;
		rti->Intersection = (void*)p;
		rti->IntersectionPointers.pFirst = p;
		for (i = 0; i < TNS_THREAD_LINE_COUNT && p; i++) {
			p = p->pNext;
		}
		rb->IntersectionManaged = p;
		rti->IntersectionPointers.pLast = p ? p->pPrev : rb->IntersectionLines.pLast;
		res = 1;
	}
	else {
		lstEmptyDirect(&rti->IntersectionPointers);
		rti->Intersection = 0;
	}

	if (rb->CreaseManaged) {
		p = rb->CreaseManaged;
		rti->Crease = (void*)p;
		rti->CreasePointers.pFirst = p;
		for (i = 0; i < TNS_THREAD_LINE_COUNT && p; i++) {
			p = p->pNext;
		}
		rb->CreaseManaged = p;
		rti->CreasePointers.pLast = p ? p->pPrev : rb->CreaseLines.pLast;
		res = 1;
	}
	else {
		lstEmptyDirect(&rti->CreasePointers);
		rti->Crease = 0;
	}

	if (rb->MaterialManaged) {
		p = rb->MaterialManaged;
		rti->Material = (void*)p;
		rti->MaterialPointers.pFirst = p;
		for (i = 0; i < TNS_THREAD_LINE_COUNT && p; i++) {
			p = p->pNext;
		}
		rb->MaterialManaged = p;
		rti->MaterialPointers.pLast = p ? p->pPrev : rb->MaterialLines.pLast;
		res = 1;
	}
	else {
		lstEmptyDirect(&rti->MaterialPointers);
		rti->Material = 0;
	}

	BLI_spin_unlock(&rb->csManagement);

	return res;
}
void lanpr_CalculateSingleLineOcclusion(LANPR_RenderBuffer* rb, LANPR_RenderLine* rl, int ThreadID) {
	real x = rl->L->FrameBufferCoord[0], y = rl->L->FrameBufferCoord[1];
	LANPR_BoundingArea* ba = lanpr_GetFirstPossibleBoundingArea(rb, rl);
	LANPR_BoundingArea* nba = ba;
	LANPR_RenderTriangleThread* rt;
	nListItemPointer* lip;
	Camera* c = rb->Scene->camera->data;
	real l, r;
	real k = (rl->R->FrameBufferCoord[1] - rl->L->FrameBufferCoord[1]) / (rl->R->FrameBufferCoord[0] - rl->L->FrameBufferCoord[0] + 1e-30);
	int PositiveX = (rl->R->FrameBufferCoord[0] - rl->L->FrameBufferCoord[0])>0 ? 1 : 0;
	int PositiveY = (rl->R->FrameBufferCoord[1] - rl->L->FrameBufferCoord[1])>0 ? 1 : 0;

	while (nba) {

		for (lip = nba->AssociatedTriangles.pFirst; lip; lip = lip->pNext) {
			rt = lip->p;
			//if (rt->Testing[ThreadID] == rl || rl->L->IntersectWith == rt || rl->R->IntersectWith == rt) continue;
			rt->Testing[ThreadID] = rl;
			if (lanpr_TriangleLineImageSpaceIntersectTestOnlyV2((void*)rt, rl, c, rb->ViewProjection, &l, &r)) {
				lanpr_CutLineIntegrated(rb, rl, l, r);
			}
		}
		nba = lanpr_GetNextBoundingArea(nba, rl, x, y, k, PositiveX, PositiveY, &x, &y);
	}
}
void THREAD_CalculateLineOcclusion(LANPR_RenderTaskInfo* rti) {
	LANPR_RenderBuffer* rb = rti->RenderBuffer;
	int ThreadId = rti->ThreadID;
	nListItemPointer* lip;
	int count = 0;

	while (lanpr_MakeNextOcclusionTaskInfo(rb, rti)) {

		for (lip = (void*)rti->Contour; lip&& lip->pPrev != rti->ContourPointers.pLast; lip = lip->pNext) {
			lanpr_CalculateSingleLineOcclusion(rb, lip->p, rti->ThreadID);

			count++;
		}
		//tnsset_PlusRenderContourProcessedCount(rb, count);
		count = 0;

		for (lip = (void*)rti->Crease; lip && lip->pPrev != rti->CreasePointers.pLast; lip = lip->pNext) {
			lanpr_CalculateSingleLineOcclusion(rb, lip->p, rti->ThreadID);
			count++;
		}
		//tnsset_PlusRenderCreaseProcessedCount(rb, count);
		count = 0;

		for (lip = (void*)rti->Intersection; lip&& lip->pPrev != rti->IntersectionPointers.pLast; lip = lip->pNext) {
			lanpr_CalculateSingleLineOcclusion(rb, lip->p, rti->ThreadID);
			count++;
		}
		//tnsset_PlusRenderIntersectionProcessedCount(rb, count);
		count = 0;

		for (lip = (void*)rti->Material; lip&& lip->pPrev != rti->MaterialPointers.pLast; lip = lip->pNext) {
			lanpr_CalculateSingleLineOcclusion(rb, lip->p, rti->ThreadID);
			count++;
		}
		//tnsset_PlusRenderMaterialProcessedCount(rb, count);
		count = 0;

	}
	//thrd_exit(0);
}

int lanpr_GetNormal(tnsVector3d v1, tnsVector3d v2, tnsVector3d v3, tnsVector3d n, tnsVector3d Pos) {
	tnsVector3d vec1, vec2;

	tMatVectorMinus3d(vec1, v2, v1);
	tMatVectorMinus3d(vec2, v3, v1);
	tMatVectorCross3d(n, vec1, vec2);
	tMatNormalizeSelf3d(n);
	if (Pos && (tMatDot3d(n, Pos, 1) < 0)) {
		tMatVectorMultiSelf3d(n, -1.0f);
		return 1;
	}
	return 0;
}

int lanpr_BoundBoxCrosses(tnsVector4d xxyy1, tnsVector4d xxyy2) {
	real XMax1 = TNS_MAX2(xxyy1[0], xxyy1[1]);
	real XMin1 = TNS_MIN2(xxyy1[0], xxyy1[1]);
	real YMax1 = TNS_MAX2(xxyy1[2], xxyy1[3]);
	real YMin1 = TNS_MIN2(xxyy1[2], xxyy1[3]);
	real XMax2 = TNS_MAX2(xxyy2[0], xxyy2[1]);
	real XMin2 = TNS_MIN2(xxyy2[0], xxyy2[1]);
	real YMax2 = TNS_MAX2(xxyy2[2], xxyy2[3]);
	real YMin2 = TNS_MIN2(xxyy2[2], xxyy2[3]);

	if (XMax1<XMin2 || XMin1>XMax2) return 0;
	if (YMax1<YMin2 || YMin1>YMax2) return 0;

	return 1;
}
int lanpr_PointInsideTrianglef(tnsVector2d v, tnsVector2d v0, tnsVector2d v1, tnsVector2d v2) {
	double cl, c;

	cl = (v0[0] - v[0]) * (v1[1] - v[1]) - (v0[1] - v[1]) * (v1[0] - v[0]);
	c = cl;

	cl = (v1[0] - v[0]) * (v2[1] - v[1]) - (v1[1] - v[1]) * (v2[0] - v[0]);
	if (c*cl <= 0)return 0;
	else c = cl;

	cl = (v2[0] - v[0]) * (v0[1] - v[1]) - (v2[1] - v[1]) * (v0[0] - v[0]);
	if (c*cl <= 0)return 0;
	else c = cl;

	cl = (v0[0] - v[0]) * (v1[1] - v[1]) - (v0[1] - v[1]) * (v1[0] - v[0]);
	if (c*cl <= 0)return 0;

	return 1;
}
int lanpr_PointOnLinef(tnsVector2d v, tnsVector2d v0, tnsVector2d v1) {
	real c1, c2;

	c1 = tMatGetLinearRatio(v0[0], v1[0], v[0]);
	c2 = tMatGetLinearRatio(v0[1], v1[1], v[1]);

	if (TNS_DOUBLE_CLOSE_ENOUGH(c1, c2) && c1 >= 0 && c1 <= 1) return 1;

	return 0;
}
int lanpr_PointTriangleRelation(tnsVector2d v, tnsVector2d v0, tnsVector2d v1, tnsVector2d v2) {
	double cl, c;
	real r;
	if (lanpr_PointOnLinef(v, v0, v1) || lanpr_PointOnLinef(v, v1, v2) || lanpr_PointOnLinef(v, v2, v0)) return 1;

	cl = (v0[0] - v[0]) * (v1[1] - v[1]) - (v0[1] - v[1]) * (v1[0] - v[0]);
	c = cl;

	cl = (v1[0] - v[0]) * (v2[1] - v[1]) - (v1[1] - v[1]) * (v2[0] - v[0]);
	if ((r = c*cl) < 0) return 0;
	elif(r == 0) return 1;
	else c = cl;

	cl = (v2[0] - v[0]) * (v0[1] - v[1]) - (v2[1] - v[1]) * (v0[0] - v[0]);
	if ((r = c*cl) < 0) return 0;
	elif(r == 0) return 1;
	else c = cl;

	cl = (v0[0] - v[0]) * (v1[1] - v[1]) - (v0[1] - v[1]) * (v1[0] - v[0]);
	if ((r = c*cl) < 0) return 0;
	elif(r == 0) return 1;

	return 2;
}
int lanpr_PointInsideTriangle3d(tnsVector3d v, tnsVector3d v0, tnsVector3d v1, tnsVector3d v2) {
	tnsVector3d L, R;
	tnsVector3d N1, N2;

	tMatVectorMinus3d(L, v1, v0);
	tMatVectorMinus3d(R, v, v1);
	tMatVectorCross3d(N1, L, R);

	tMatVectorMinus3d(L, v2, v1);
	tMatVectorMinus3d(R, v, v2);
	tMatVectorCross3d(N2, L, R);

	if (tMatDot3d(N1, N2, 0) < 0) return 0;

	tMatVectorMinus3d(L, v0, v2);
	tMatVectorMinus3d(R, v, v0);
	tMatVectorCross3d(N1, L, R);

	if (tMatDot3d(N1, N2, 0) < 0) return 0;

	tMatVectorMinus3d(L, v1, v0);
	tMatVectorMinus3d(R, v, v1);
	tMatVectorCross3d(N2, L, R);

	if (tMatDot3d(N1, N2, 0) < 0) return 0;

	return 1;
}
int lanpr_PointInsideTriangle3de(tnsVector3d v, tnsVector3d v0, tnsVector3d v1, tnsVector3d v2) {
	tnsVector3d L, R;
	tnsVector3d N1, N2;
	real d;

	tMatVectorMinus3d(L, v1, v0);
	tMatVectorMinus3d(R, v, v1);
	//tMatNormalizeSelf3d(L);
	//tMatNormalizeSelf3d(R);
	tMatVectorCross3d(N1, L, R);

	tMatVectorMinus3d(L, v2, v1);
	tMatVectorMinus3d(R, v, v2);
	//tMatNormalizeSelf3d(L);
	//tMatNormalizeSelf3d(R);
	tMatVectorCross3d(N2, L, R);

	if ((d = tMatDot3d(N1, N2, 0)) < 0)return 0;
	//if (d<DBL_EPSILON) return -1;

	tMatVectorMinus3d(L, v0, v2);
	tMatVectorMinus3d(R, v, v0);
	//tMatNormalizeSelf3d(L);
	//tMatNormalizeSelf3d(R);
	tMatVectorCross3d(N1, L, R);

	if ((d = tMatDot3d(N1, N2, 0)) < 0) return 0;
	//if (d<DBL_EPSILON) return -1;

	tMatVectorMinus3d(L, v1, v0);
	tMatVectorMinus3d(R, v, v1);
	//tMatNormalizeSelf3d(L);
	//tMatNormalizeSelf3d(R);
	tMatVectorCross3d(N2, L, R);

	if ((d = tMatDot3d(N1, N2, 0)) < 0) return 0;
	//if (d<DBL_EPSILON) return -1;

	return 1;
}

LANPR_RenderElementLinkNode* lanpr_NewCullTriangleSpace64(LANPR_RenderBuffer* rb) {
	LANPR_RenderElementLinkNode* reln;

	LANPR_RenderTriangle* RenderTriangles = calloc(64, rb->TriangleSize);//CreateNewBuffer(LANPR_RenderTriangle, 64);

	reln = lstAppendPointerStaticSized(&rb->TriangleBufferPointers, &rb->RenderDataPool, RenderTriangles,
		sizeof(LANPR_RenderElementLinkNode));
	reln->ElementCount = 64;
	reln->Additional = 1;

	return reln;
}
LANPR_RenderElementLinkNode* lanpr_NewCullPointSpace64(LANPR_RenderBuffer* rb) {
	LANPR_RenderElementLinkNode* reln;

	LANPR_RenderVert* RenderVertices = CreateNewBuffer(LANPR_RenderVert, 64);

	reln = lstAppendPointerStaticSized(&rb->VertexBufferPointers, &rb->RenderDataPool, RenderVertices,
		sizeof(LANPR_RenderElementLinkNode));
	reln->ElementCount = 64;
	reln->Additional = 1;

	return reln;
}
void lanpr_CalculateRenderTriangleNormal(LANPR_RenderTriangle* rt);
void lanpr_PostTriangle(LANPR_RenderTriangle* rt, LANPR_RenderTriangle* orig) {
	if (rt->V[0])tMatVectorAccum3d(rt->GC, rt->V[0]->FrameBufferCoord);
	if (rt->V[1])tMatVectorAccum3d(rt->GC, rt->V[1]->FrameBufferCoord);
	if (rt->V[2])tMatVectorAccum3d(rt->GC, rt->V[2]->FrameBufferCoord);
	tMatVectorMultiSelf3d(rt->GC, 1.0f / 3.0f);

	tMatVectorCopy3d(orig->GN, rt->GN);
}
void lanpr_CullTriangles(LANPR_RenderBuffer* rb) {
	LANPR_RenderLine* rl;
	LANPR_RenderTriangle* rt, *rt1;
	LANPR_RenderVert* rv;
	LANPR_RenderElementLinkNode* reln, *veln, *teln;
	LANPR_RenderLineSegment* rls;
	real* MVInverse = rb->VPInverse;
	int i;
	real a;
	int VCount = 0, TCount = 0;
	Object* o;

	veln = lanpr_NewCullPointSpace64(rb);
	teln = lanpr_NewCullTriangleSpace64(rb);
	rv = &((LANPR_RenderVert*)veln->Pointer)[VCount];
	rt1 = (void*)(((BYTE*)teln->Pointer) + rb->TriangleSize*TCount);

	for (reln = rb->TriangleBufferPointers.pFirst; reln; reln = reln->Item.pNext) {
		i = 0;
		if (reln->Additional) continue;
		o = reln->ObjectRef;
		for (i; i < reln->ElementCount; i++) {
			int In1 = 0, In2 = 0, In3 = 0;
			rt = (void*)(((BYTE*)reln->Pointer) + rb->TriangleSize*i);
			if (rt->V[0]->FrameBufferCoord[3] < 0) In1 = 1;
			if (rt->V[1]->FrameBufferCoord[3] < 0) In2 = 1;
			if (rt->V[2]->FrameBufferCoord[3] < 0) In3 = 1;

			rt->RL[0]->ObjectRef = o;
			rt->RL[1]->ObjectRef = o;
			rt->RL[2]->ObjectRef = o;

			if (VCount > 60) {
				veln->ElementCount = VCount;
				veln = lanpr_NewCullPointSpace64(rb);
				VCount = 0;
			}

			if (TCount > 60) {
				teln->ElementCount = TCount;
				teln = lanpr_NewCullTriangleSpace64(rb);
				TCount = 0;
			}

			if ((!rt->RL[0]->Item.pNext && !rt->RL[0]->Item.pPrev) ||
				(!rt->RL[1]->Item.pNext && !rt->RL[1]->Item.pPrev) ||
				(!rt->RL[2]->Item.pNext && !rt->RL[2]->Item.pPrev)) {
				printf("'");
			}

			rv = &((LANPR_RenderVert*)veln->Pointer)[VCount];
			rt1 = &((LANPR_RenderTriangle*)teln->Pointer)[TCount];


			switch (In1 + In2 + In3) {
			case 0:
				continue;
			case 3:
				rt->CullStatus = TNS_CULL_DISCARD;
				continue;
			case 2:
				rt->CullStatus = TNS_CULL_USED;
				if (!In1) {
					a = rt->V[0]->FrameBufferCoord[2] / (rt->V[0]->FrameBufferCoord[2] - rt->V[2]->FrameBufferCoord[2]);
					rv[0].FrameBufferCoord[0] = (1 - a)*rt->V[0]->FrameBufferCoord[0] + a* rt->V[2]->FrameBufferCoord[0];
					rv[0].FrameBufferCoord[1] = (1 - a)*rt->V[0]->FrameBufferCoord[1] + a* rt->V[2]->FrameBufferCoord[1];
					rv[0].FrameBufferCoord[2] = 0;
					rv[0].FrameBufferCoord[3] = (1 - a)*rt->V[0]->FrameBufferCoord[3] + a* rt->V[2]->FrameBufferCoord[3];
					tMatApplyTransform44dTrue(rv[0].GLocation, MVInverse, rv[0].FrameBufferCoord);

					a = rt->V[0]->FrameBufferCoord[2] / (rt->V[0]->FrameBufferCoord[2] - rt->V[1]->FrameBufferCoord[2]);
					rv[1].FrameBufferCoord[0] = (1 - a)*rt->V[0]->FrameBufferCoord[0] + a* rt->V[1]->FrameBufferCoord[0];
					rv[1].FrameBufferCoord[1] = (1 - a)*rt->V[0]->FrameBufferCoord[1] + a* rt->V[1]->FrameBufferCoord[1];
					rv[1].FrameBufferCoord[2] = 0;
					rv[1].FrameBufferCoord[3] = (1 - a)*rt->V[0]->FrameBufferCoord[3] + a* rt->V[1]->FrameBufferCoord[3];
					tMatApplyTransform44dTrue(rv[1].GLocation, MVInverse, rv[1].FrameBufferCoord);

					lstRemoveItem(&rb->AllRenderLines, (void*)rt->RL[0]);
					lstRemoveItem(&rb->AllRenderLines, (void*)rt->RL[1]);
					lstRemoveItem(&rb->AllRenderLines, (void*)rt->RL[2]);

					rl = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLine));
					rls = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
					lstAppendItem(&rl->Segments, rls);
					lstAppendItem(&rb->AllRenderLines, rl);
					rl->L = &rv[1];
					rl->R = &rv[0];
					rl->TL = rt1;
					rt1->RL[1] = rl;

					rl = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLine));
					rls = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
					lstAppendItem(&rl->Segments, rls);
					lstAppendItem(&rb->AllRenderLines, rl);
					rl->L = &rv[1];
					rl->R = rt->V[0];
					rl->TL = rt->RL[0]->TL == rt ? rt1 : rt->RL[0]->TL;
					rl->TR = rt->RL[0]->TR == rt ? rt1 : rt->RL[0]->TR;
					rt1->RL[0] = rl;

					rl = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLine));
					rls = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
					lstAppendItem(&rl->Segments, rls);
					lstAppendItem(&rb->AllRenderLines, rl);
					rl->L = rt->V[0];
					rl->R = &rv[0];
					rl->TL = rt->RL[2]->TL == rt ? rt1 : rt->RL[2]->TL;
					rl->TR = rt->RL[2]->TR == rt ? rt1 : rt->RL[2]->TR;
					rt1->RL[2] = rl;

					rt1->V[0] = rt->V[0];
					rt1->V[1] = &rv[1];
					rt1->V[2] = &rv[0];

					lanpr_PostTriangle(rt1, rt);

					VCount += 2;
					TCount += 1;
					continue;
				}elif(!In3) {
					a = rt->V[2]->FrameBufferCoord[2] / (rt->V[2]->FrameBufferCoord[2] - rt->V[0]->FrameBufferCoord[2]);
					rv[0].FrameBufferCoord[0] = (1 - a)*rt->V[2]->FrameBufferCoord[0] + a* rt->V[0]->FrameBufferCoord[0];
					rv[0].FrameBufferCoord[1] = (1 - a)*rt->V[2]->FrameBufferCoord[1] + a* rt->V[0]->FrameBufferCoord[1];
					rv[0].FrameBufferCoord[2] = 0;
					rv[0].FrameBufferCoord[3] = (1 - a)*rt->V[2]->FrameBufferCoord[3] + a* rt->V[0]->FrameBufferCoord[3];
					tMatApplyTransform44dTrue(rv[0].GLocation, MVInverse, rv[0].FrameBufferCoord);

					a = rt->V[2]->FrameBufferCoord[2] / (rt->V[2]->FrameBufferCoord[2] - rt->V[1]->FrameBufferCoord[2]);
					rv[1].FrameBufferCoord[0] = (1 - a)*rt->V[2]->FrameBufferCoord[0] + a* rt->V[1]->FrameBufferCoord[0];
					rv[1].FrameBufferCoord[1] = (1 - a)*rt->V[2]->FrameBufferCoord[1] + a* rt->V[1]->FrameBufferCoord[1];
					rv[1].FrameBufferCoord[2] = 0;
					rv[1].FrameBufferCoord[3] = (1 - a)*rt->V[2]->FrameBufferCoord[3] + a* rt->V[1]->FrameBufferCoord[3];
					tMatApplyTransform44dTrue(rv[1].GLocation, MVInverse, rv[1].FrameBufferCoord);

					lstRemoveItem(&rb->AllRenderLines, (void*)rt->RL[0]);
					lstRemoveItem(&rb->AllRenderLines, (void*)rt->RL[1]);
					lstRemoveItem(&rb->AllRenderLines, (void*)rt->RL[2]);

					rl = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLine));
					rls = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
					lstAppendItem(&rl->Segments, rls);
					lstAppendItem(&rb->AllRenderLines, rl);
					rl->L = &rv[0];
					rl->R = &rv[1];
					rl->TL = rt1;
					rt1->RL[0] = rl;

					rl = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLine));
					rls = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
					lstAppendItem(&rl->Segments, rls);
					lstAppendItem(&rb->AllRenderLines, rl);
					rl->L = &rv[1];
					rl->R = rt->V[2];
					rl->TL = rt->RL[1]->TL == rt ? rt1 : rt->RL[1]->TL;
					rl->TR = rt->RL[1]->TR == rt ? rt1 : rt->RL[1]->TR;
					rt1->RL[1] = rl;

					rl = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLine));
					rls = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
					lstAppendItem(&rl->Segments, rls);
					lstAppendItem(&rb->AllRenderLines, rl);
					rl->L = rt->V[2];
					rl->R = &rv[0];
					rl->TL = rt->RL[2]->TL == rt ? rt1 : rt->RL[2]->TL;
					rl->TR = rt->RL[2]->TR == rt ? rt1 : rt->RL[2]->TR;
					rt1->RL[2] = rl;

					rt1->V[0] = &rv[1];
					rt1->V[1] = rt->V[2];
					rt1->V[2] = &rv[0];

					lanpr_PostTriangle(rt1, rt);

					VCount += 2;
					TCount += 1;
					continue;
				}elif(!In2) {
					a = rt->V[1]->FrameBufferCoord[2] / (rt->V[1]->FrameBufferCoord[2] - rt->V[0]->FrameBufferCoord[2]);
					rv[0].FrameBufferCoord[0] = (1 - a)*rt->V[1]->FrameBufferCoord[0] + a* rt->V[0]->FrameBufferCoord[0];
					rv[0].FrameBufferCoord[1] = (1 - a)*rt->V[1]->FrameBufferCoord[1] + a* rt->V[0]->FrameBufferCoord[1];
					rv[0].FrameBufferCoord[2] = 0;
					rv[0].FrameBufferCoord[3] = (1 - a)*rt->V[1]->FrameBufferCoord[3] + a* rt->V[0]->FrameBufferCoord[3];
					tMatApplyTransform44dTrue(rv[0].GLocation, MVInverse, rv[0].FrameBufferCoord);

					a = rt->V[1]->FrameBufferCoord[2] / (rt->V[1]->FrameBufferCoord[2] - rt->V[2]->FrameBufferCoord[2]);
					rv[1].FrameBufferCoord[0] = (1 - a)*rt->V[1]->FrameBufferCoord[0] + a* rt->V[2]->FrameBufferCoord[0];
					rv[1].FrameBufferCoord[1] = (1 - a)*rt->V[1]->FrameBufferCoord[1] + a* rt->V[2]->FrameBufferCoord[1];
					rv[1].FrameBufferCoord[2] = 0;
					rv[1].FrameBufferCoord[3] = (1 - a)*rt->V[1]->FrameBufferCoord[3] + a* rt->V[2]->FrameBufferCoord[3];
					tMatApplyTransform44dTrue(rv[1].GLocation, MVInverse, rv[1].FrameBufferCoord);

					lstRemoveItem(&rb->AllRenderLines, (void*)rt->RL[0]);
					lstRemoveItem(&rb->AllRenderLines, (void*)rt->RL[1]);
					lstRemoveItem(&rb->AllRenderLines, (void*)rt->RL[2]);

					rl = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLine));
					rls = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
					lstAppendItem(&rl->Segments, rls);
					lstAppendItem(&rb->AllRenderLines, rl);
					rl->L = &rv[1];
					rl->R = &rv[0];
					rl->TL = rt1;
					rt1->RL[2] = rl;

					rl = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLine));
					rls = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
					lstAppendItem(&rl->Segments, rls);
					lstAppendItem(&rb->AllRenderLines, rl);
					rl->L = &rv[0];
					rl->R = rt->V[1];
					rl->TL = rt->RL[0]->TL == rt ? rt1 : rt->RL[0]->TL;
					rl->TR = rt->RL[0]->TR == rt ? rt1 : rt->RL[0]->TR;
					rt1->RL[0] = rl;

					rl = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLine));
					rls = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
					lstAppendItem(&rl->Segments, rls);
					lstAppendItem(&rb->AllRenderLines, rl);
					rl->L = rt->V[1];
					rl->R = &rv[1];
					rl->TL = rt->RL[1]->TL == rt ? rt1 : rt->RL[1]->TL;
					rl->TR = rt->RL[1]->TR == rt ? rt1 : rt->RL[1]->TR;
					rt1->RL[1] = rl;

					rt1->V[0] = rt->V[1];
					rt1->V[1] = &rv[1];
					rt1->V[2] = &rv[0];

					lanpr_PostTriangle(rt1, rt);

					VCount += 2;
					TCount += 1;
					continue;
				}
				break;
			case 1:
				rt->CullStatus = TNS_CULL_USED;
				if (In1) {
					a = rt->V[0]->FrameBufferCoord[2] / (rt->V[0]->FrameBufferCoord[2] - rt->V[2]->FrameBufferCoord[2]);
					rv[0].FrameBufferCoord[0] = (1 - a)*rt->V[0]->FrameBufferCoord[0] + a* rt->V[2]->FrameBufferCoord[0];
					rv[0].FrameBufferCoord[1] = (1 - a)*rt->V[0]->FrameBufferCoord[1] + a* rt->V[2]->FrameBufferCoord[1];
					rv[0].FrameBufferCoord[2] = 0;
					rv[0].FrameBufferCoord[3] = (1 - a)*rt->V[0]->FrameBufferCoord[3] + a* rt->V[2]->FrameBufferCoord[3];
					tMatApplyTransform44dTrue(rv[0].GLocation, MVInverse, rv[0].FrameBufferCoord);

					a = rt->V[0]->FrameBufferCoord[2] / (rt->V[0]->FrameBufferCoord[2] - rt->V[1]->FrameBufferCoord[2]);
					rv[1].FrameBufferCoord[0] = (1 - a)*rt->V[0]->FrameBufferCoord[0] + a* rt->V[1]->FrameBufferCoord[0];
					rv[1].FrameBufferCoord[1] = (1 - a)*rt->V[0]->FrameBufferCoord[1] + a* rt->V[1]->FrameBufferCoord[1];
					rv[1].FrameBufferCoord[2] = 0;
					rv[1].FrameBufferCoord[3] = (1 - a)*rt->V[0]->FrameBufferCoord[3] + a* rt->V[1]->FrameBufferCoord[3];
					tMatApplyTransform44dTrue(rv[1].GLocation, MVInverse, rv[1].FrameBufferCoord);

					lstRemoveItem(&rb->AllRenderLines, (void*)rt->RL[0]);
					lstRemoveItem(&rb->AllRenderLines, (void*)rt->RL[2]);

					rl = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLine));
					rls = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
					lstAppendItem(&rl->Segments, rls);
					lstAppendItem(&rb->AllRenderLines, rl);
					rl->L = &rv[1];
					rl->R = &rv[0];
					rl->TL = rt1;
					rt1[0].RL[1] = rl;

					rl = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLine));
					rls = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
					lstAppendItem(&rl->Segments, rls);
					lstAppendItem(&rb->AllRenderLines, rl);
					rl->L = &rv[0];
					rl->R = rt->V[1];
					rl->TL = &rt1[0];
					rl->TR = &rt1[1];
					rt1[0].RL[2] = rl;
					rt1[1].RL[0] = rl;

					rl = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLine));
					rls = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
					lstAppendItem(&rl->Segments, rls);
					lstAppendItem(&rb->AllRenderLines, rl);
					rl->L = rt->V[1];
					rl->R = &rv[1];
					rl->TL = rt->RL[0]->TL == rt ? rt1 : rt->RL[0]->TL;
					rl->TR = rt->RL[0]->TR == rt ? rt1 : rt->RL[0]->TR;
					rt1[0].RL[0] = rl;

					rt1[0].V[0] = rt->V[1];
					rt1[0].V[1] = &rv[1];
					rt1[0].V[2] = &rv[0];

					rl = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLine));
					rls = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
					lstAppendItem(&rl->Segments, rls);
					lstAppendItem(&rb->AllRenderLines, rl);
					rl->L = rt->V[2];
					rl->R = &rv[0];
					rl->TL = rt->RL[2]->TL == rt ? rt1 : rt->RL[2]->TL;
					rl->TR = rt->RL[2]->TR == rt ? rt1 : rt->RL[2]->TR;
					rt1[1].RL[2] = rl;
					rt1[1].RL[1] = rt->RL[1];

					rt1[1].V[0] = &rv[0];
					rt1[1].V[1] = rt->V[1];
					rt1[1].V[2] = rt->V[2];

					lanpr_PostTriangle(&rt1[0], rt);
					lanpr_PostTriangle(&rt1[1], rt);

					VCount += 2;
					TCount += 2;
					continue;
				}elif(In2) {
					a = rt->V[1]->FrameBufferCoord[2] / (rt->V[1]->FrameBufferCoord[2] - rt->V[0]->FrameBufferCoord[2]);
					rv[0].FrameBufferCoord[0] = (1 - a)*rt->V[1]->FrameBufferCoord[0] + a* rt->V[0]->FrameBufferCoord[0];
					rv[0].FrameBufferCoord[1] = (1 - a)*rt->V[1]->FrameBufferCoord[1] + a* rt->V[0]->FrameBufferCoord[1];
					rv[0].FrameBufferCoord[2] = 0;
					rv[0].FrameBufferCoord[3] = (1 - a)*rt->V[1]->FrameBufferCoord[3] + a* rt->V[0]->FrameBufferCoord[3];
					tMatApplyTransform44dTrue(rv[0].GLocation, MVInverse, rv[0].FrameBufferCoord);

					a = rt->V[1]->FrameBufferCoord[2] / (rt->V[1]->FrameBufferCoord[2] - rt->V[2]->FrameBufferCoord[2]);
					rv[1].FrameBufferCoord[0] = (1 - a)*rt->V[1]->FrameBufferCoord[0] + a* rt->V[2]->FrameBufferCoord[0];
					rv[1].FrameBufferCoord[1] = (1 - a)*rt->V[1]->FrameBufferCoord[1] + a* rt->V[2]->FrameBufferCoord[1];
					rv[1].FrameBufferCoord[2] = 0;
					rv[1].FrameBufferCoord[3] = (1 - a)*rt->V[1]->FrameBufferCoord[3] + a* rt->V[2]->FrameBufferCoord[3];
					tMatApplyTransform44dTrue(rv[1].GLocation, MVInverse, rv[1].FrameBufferCoord);

					lstRemoveItem(&rb->AllRenderLines, (void*)rt->RL[0]);
					lstRemoveItem(&rb->AllRenderLines, (void*)rt->RL[1]);

					rl = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLine));
					rls = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
					lstAppendItem(&rl->Segments, rls);
					lstAppendItem(&rb->AllRenderLines, rl);
					rl->L = &rv[1];
					rl->R = &rv[0];
					rl->TL = rt1;
					rt1[0].RL[1] = rl;

					rl = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLine));
					rls = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
					lstAppendItem(&rl->Segments, rls);
					lstAppendItem(&rb->AllRenderLines, rl);
					rl->L = &rv[0];
					rl->R = rt->V[2];
					rl->TL = &rt1[0];
					rl->TR = &rt1[1];
					rt1[0].RL[2] = rl;
					rt1[1].RL[0] = rl;

					rl = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLine));
					rls = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
					lstAppendItem(&rl->Segments, rls);
					lstAppendItem(&rb->AllRenderLines, rl);
					rl->L = rt->V[2];
					rl->R = &rv[1];
					rl->TL = rt->RL[1]->TL == rt ? rt1 : rt->RL[1]->TL;
					rl->TR = rt->RL[1]->TR == rt ? rt1 : rt->RL[1]->TR;
					rt1[0].RL[0] = rl;

					rt1[0].V[0] = rt->V[2];
					rt1[0].V[1] = &rv[1];
					rt1[0].V[2] = &rv[0];

					rl = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLine));
					rls = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
					lstAppendItem(&rl->Segments, rls);
					lstAppendItem(&rb->AllRenderLines, rl);
					rl->L = rt->V[0];
					rl->R = &rv[0];
					rl->TL = rt->RL[0]->TL == rt ? rt1 : rt->RL[0]->TL;
					rl->TR = rt->RL[0]->TR == rt ? rt1 : rt->RL[0]->TR;
					rt1[1].RL[2] = rl;
					rt1[1].RL[1] = rt->RL[2];

					rt1[1].V[0] = &rv[0];
					rt1[1].V[1] = rt->V[2];
					rt1[1].V[2] = rt->V[0];

					lanpr_PostTriangle(&rt1[0], rt);
					lanpr_PostTriangle(&rt1[1], rt);

					VCount += 2;
					TCount += 2;
					continue;
				}elif(In3) {
					a = rt->V[2]->FrameBufferCoord[2] / (rt->V[2]->FrameBufferCoord[2] - rt->V[0]->FrameBufferCoord[2]);
					rv[0].FrameBufferCoord[0] = (1 - a)*rt->V[2]->FrameBufferCoord[0] + a* rt->V[0]->FrameBufferCoord[0];
					rv[0].FrameBufferCoord[1] = (1 - a)*rt->V[2]->FrameBufferCoord[1] + a* rt->V[0]->FrameBufferCoord[1];
					rv[0].FrameBufferCoord[2] = 0;
					rv[0].FrameBufferCoord[3] = (1 - a)*rt->V[2]->FrameBufferCoord[3] + a* rt->V[0]->FrameBufferCoord[3];
					tMatApplyTransform44dTrue(rv[0].GLocation, MVInverse, rv[0].FrameBufferCoord);

					a = rt->V[2]->FrameBufferCoord[2] / (rt->V[2]->FrameBufferCoord[2] - rt->V[1]->FrameBufferCoord[2]);
					rv[1].FrameBufferCoord[0] = (1 - a)*rt->V[2]->FrameBufferCoord[0] + a* rt->V[1]->FrameBufferCoord[0];
					rv[1].FrameBufferCoord[1] = (1 - a)*rt->V[2]->FrameBufferCoord[1] + a* rt->V[1]->FrameBufferCoord[1];
					rv[1].FrameBufferCoord[2] = 0;
					rv[1].FrameBufferCoord[3] = (1 - a)*rt->V[2]->FrameBufferCoord[3] + a* rt->V[1]->FrameBufferCoord[3];
					tMatApplyTransform44dTrue(rv[1].GLocation, MVInverse, rv[1].FrameBufferCoord);

					lstRemoveItem(&rb->AllRenderLines, (void*)rt->RL[1]);
					lstRemoveItem(&rb->AllRenderLines, (void*)rt->RL[2]);

					rl = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLine));
					rls = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
					lstAppendItem(&rl->Segments, rls);
					lstAppendItem(&rb->AllRenderLines, rl);
					rl->L = &rv[1];
					rl->R = &rv[0];
					rl->TL = rt1;
					rt1[0].RL[1] = rl;

					rl = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLine));
					rls = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
					lstAppendItem(&rl->Segments, rls);
					lstAppendItem(&rb->AllRenderLines, rl);
					rl->L = &rv[0];
					rl->R = rt->V[0];
					rl->TL = &rt1[0];
					rl->TR = &rt1[1];
					rt1[0].RL[2] = rl;
					rt1[1].RL[0] = rl;

					rl = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLine));
					rls = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
					lstAppendItem(&rl->Segments, rls);
					lstAppendItem(&rb->AllRenderLines, rl);
					rl->L = rt->V[2];
					rl->R = &rv[1];
					rl->TL = rt->RL[0]->TL == rt ? rt1 : rt->RL[0]->TL;
					rl->TR = rt->RL[0]->TR == rt ? rt1 : rt->RL[0]->TR;
					rt1[0].RL[0] = rl;

					rt1[0].V[0] = rt->V[0];
					rt1[0].V[1] = &rv[1];
					rt1[0].V[2] = &rv[0];

					rl = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLine));
					rls = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
					lstAppendItem(&rl->Segments, rls);
					lstAppendItem(&rb->AllRenderLines, rl);
					rl->L = rt->V[1];
					rl->R = &rv[0];
					rl->TL = rt->RL[1]->TL == rt ? rt1 : rt->RL[1]->TL;
					rl->TR = rt->RL[1]->TR == rt ? rt1 : rt->RL[1]->TR;
					rt1[1].RL[2] = rl;
					rt1[1].RL[1] = rt->RL[1];

					rt1[1].V[0] = &rv[0];
					rt1[1].V[1] = rt->V[1];
					rt1[1].V[2] = rt->V[2];

					lanpr_PostTriangle(&rt1[0], rt);
					lanpr_PostTriangle(&rt1[1], rt);

					VCount += 2;
					TCount += 2;
					continue;
				}
				break;
			}
		}
		teln->ElementCount = TCount;
		veln->ElementCount = VCount;
	}
}
void lanpr_PerspectiveDivision(LANPR_RenderBuffer* rb) {
	LANPR_RenderVert* rv;
	LANPR_RenderElementLinkNode* reln;
	Camera* cam = rb->Scene->camera->data;
	int i;

	if (cam->type == CAM_PERSP) return;

	for (reln = rb->VertexBufferPointers.pFirst; reln; reln = reln->Item.pNext) {
		rv = reln->Pointer;
		for (i = 0; i < reln->ElementCount; i++) {
			//if (rv->FrameBufferCoord[2] < -DBL_EPSILON) continue;
			tMatVectorMultiSelf3d(rv[i].FrameBufferCoord, 1 / rv[i].FrameBufferCoord[3]);
			rv[i].FrameBufferCoord[2] = cam->clipsta*cam->clipend / (cam->clipend - fabs(rv[i].FrameBufferCoord[2]) * (cam->clipend - cam->clipsta));
		}
	}
}

void lanpr_TransformRenderVert(BMVert* V, LANPR_RenderVert* RVBuf, real* MVMat, real* MVPMat, Camera* Camera) {//real HeightMultiply, real clipsta, real clipend) {
	LANPR_RenderVert* rv = &RVBuf[V->I];
	rv->V = V;
	V->RV = rv;
	tMatApplyTransform43df(rv->GLocation, MVMat, V->co);
	tMatApplyTransform43df(rv->FrameBufferCoord, MVPMat, V->co);

	//if(rv->FrameBufferCoord[2]>0)tMatVectorMultiSelf3d(rv->FrameBufferCoord, (1 / rv->FrameBufferCoord[3]));
	//else tMatVectorMultiSelf3d(rv->FrameBufferCoord, -rv->FrameBufferCoord[3]);
	//   rv->FrameBufferCoord[2] = Camera->clipsta* Camera->clipend / (Camera->clipend - fabs(rv->FrameBufferCoord[2]) * (Camera->clipend - Camera->clipsta));
}
void lanpr_CalculateRenderTriangleNormal(LANPR_RenderTriangle* rt) {
	tnsVector3d L, R;
	tMatVectorMinus3d(L, rt->V[1]->GLocation, rt->V[0]->GLocation);
	tMatVectorMinus3d(R, rt->V[2]->GLocation, rt->V[0]->GLocation);
	tMatVectorCross3d(rt->GN, L, R);
	tMatNormalizeSelf3d(rt->GN);
}
void lanpr_MakeRenderGeometryBuffersObject(Object* o, real* MVMat, real* MVPMat, LANPR_RenderBuffer* rb, real HeightMultiply) {
	Object* oc;
	Mesh* mo;
	BMVert* v;
	BMFace* f;
	BMEdge* e;
	LANPR_RenderTriangle* rt;
	tnsMatrix44d NewMVP;
	tnsMatrix44d NewMV;
	LANPR_RenderBuffer* fb = rb->FrameBuffer;
	LANPR_RenderElementLinkNode* reln;
	Camera* c = rb->Scene->camera->data;
	Material* m;

	//if (o->RenderTriangles) FreeMem(o->RenderTriangles);
	//if (o->RenderVertices) FreeMem(o->RenderVertices);

	tMatMultiply44d(NewMVP, MVPMat, o->SelfTransform);
	tMatMultiply44d(NewMV, MVMat, o->SelfTransform);

	if (o->Type == TNS_OBJECT_MESH) {
		mo = o;
		o->RenderVertices = CreateNewBuffer(LANPR_RenderVert, mo->totvert);
		o->RenderTriangles = calloc(mo->TriangleCount, rb->TriangleSize);//CreateNewBuffer(LANPR_RenderTriangle, mo->TriangleCount);
																		 //o->RenderLines = CreateNewBuffer(LANPR_RenderLine, mo->TriangulatedEdgeCount);

		reln = lstAppendPointerStaticSized(&rb->VertexBufferPointers, &rb->RenderDataPool, o->RenderVertices,
			sizeof(LANPR_RenderElementLinkNode));
		reln->ElementCount = mo->totvert;
		reln->ObjectRef = mo;

		reln = lstAppendPointerStaticSized(&rb->TriangleBufferPointers, &rb->RenderDataPool, o->RenderTriangles,
			sizeof(LANPR_RenderElementLinkNode));
		reln->ElementCount = mo->TriangleCount;
		reln->ObjectRef = mo;

		for (v = mo->V.pFirst; v; v = v->Item.pNext) {
			lanpr_TransformRenderVert(v, o->RenderVertices, NewMV, NewMVP, c);//,HeightMultiply,c->clipsta,c->clipend);
			//tObjRecalculateVertNormal(v);
		}

		for (e = mo->E.pFirst; e; e = e->Item.pNext) {
			LANPR_RenderLine* rl = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLine));
			rl->L = e->VL->RV;
			rl->R = e->VR->RV;
			e->RenderLine = rl;
			LANPR_RenderLineSegment* rls = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
			lstAppendItem(&rl->Segments, rls);
			lstAppendItem(&rb->AllRenderLines, rl);
		}


		rt = o->RenderTriangles;
		for (f = mo->F.pFirst; f; f = f->Item.pNext) {
			//tObjRecalculateFaceAverageNormal(f);
			//tObjSimpleTriangulateRender(f, rt, rb->TriangleSize, o->RenderVertices, &rb->AllRenderLines, &rt, &rb->RenderDataPool);
			// already done in the func above. lanpr_CalculateRenderTriangleNormal(rt);
			tMatApplyNormalTransform43d(f->GNormal, MVMat, f->FaceNormal);
			tMatNormalizeSelf3d(f->GNormal);
			//m = tnsGetIndexedMaterial(rb->Scene, f->MaterialID);
			//if(m) m->PreviewVCount += (f->TriangleCount*3);
		}
	}

	for (oc = o->ChildObjects.pFirst; oc; oc = oc->Item.pNext) {
		lanpr_MakeRenderGeometryBuffersRecursive(oc, NewMV, NewMVP, rb);
	}
}
void lanpr_MakeRenderGeometryBuffers(Scene* s, Object* c/*camera*/, LANPR_RenderBuffer* rb) {
	Object* o;
	Collection* collection;
	CollectionObject* co;
	tnsMatrix44d obmat16;
	tnsMatrix44d proj, view, result, inv;
	if (!c) return;

	memset(rb->MaterialPointers, 0, sizeof(void*) * 2048);

	real asp = ((real)rb->W / (real)rb->H);

	if (c->type == CAM_PERSP) {
		tMatMakePerspectiveMatrix44d(proj, c->FOV, asp, c->clipsta, c->clipend);
	}elif(c->type == CAM_ORTHO) {
		real w = c->OrthScale;
		tMatMakeOrthographicMatrix44d(proj, -w, w, -w / asp, w / asp, c->clipsta, c->clipend);
	}

	tMatLoadIdentity44d(view);

	//tObjApplySelfTransformMatrix(c, 0);
	//tObjApplyGlobalTransformMatrixReverted(c);
	tMatObmatTo16d(c->obmat, obmat16)
	tMatInverse44d(inv, obmat16);
	tMatMultiply44d(result, proj, inv);
	memcpy(proj, result, sizeof(tnsMatrix44d));
	memcpy(rb->ViewProjection, proj, sizeof(tnsMatrix44d));

	tMatInverse44d(rb->VPInverse, rb->ViewProjection);

	void* a;
	while (a = lstPopPointer(&rb->TriangleBufferPointers)) FreeMem(a);
	while (a = lstPopPointer(&rb->VertexBufferPointers)) FreeMem(a);

	for (collection = s->master_collection.first; collection; collection = collection->ID.next) {
		for (co = collection->gobject.first; co; co = co->next) {
			//tObjApplyGlobalTransformMatrixRecursive(o);
			lanpr_MakeRenderGeometryBuffersObject(o, view, proj, rb);
		}
	}
}



#define INTERSECT_SORT_MIN_TO_MAX_3(ia,ib,ic,lst)\
{\
lst[0] = TNS_MIN3_INDEX(ia,ib,ic);\
lst[1] = ( ((ia<=ib && ib<=ic)||(ic<=ib && ib<=ia)) ? 1 : (((ic<=ia && ia<=ib)||(ib<ia && ia<=ic))? 0 : 2));\
lst[2] = TNS_MAX3_INDEX(ia,ib,ic);\
}

//ia ib ic are ordered
#define INTERSECT_JUST_GREATER(is,order,num,index)\
{\
index = (num<is[order[0]]?order[0]:(num<is[order[1]]?order[1]:(num<is[order[2]]?order[2]:order[2])));\
}

//ia ib ic are ordered
#define INTERSECT_JUST_SMALLER(is,order,num,index)\
{\
index = (num>is[order[2]]?order[2]:(num>is[order[1]]?order[1]:(num>is[order[0]]?order[0]:order[0])));\
}

void lanpr_GetInterpolatePoint2d(tnsVector2d L, tnsVector2d R, real Ratio, tnsVector2d Result) {
	Result[0] = tnsLinearItp(L[0], R[0], Ratio);
	Result[1] = tnsLinearItp(L[1], R[1], Ratio);
}
void lanpr_GetInterpolatePoint3d(tnsVector2d L, tnsVector2d R, real Ratio, tnsVector2d Result) {
	Result[0] = tnsLinearItp(L[0], R[0], Ratio);
	Result[1] = tnsLinearItp(L[1], R[1], Ratio);
	Result[2] = tnsLinearItp(L[2], R[2], Ratio);
}

int tRdtGetZIntersectPoint(tnsVector3d TL, tnsVector3d TR, tnsVector3d LL, tnsVector3d LR, tnsVector3d IntersectResult) {
	//real lzl = 1 / LL[2], lzr = 1 / LR[2], tzl = 1 / TL[2], tzr = 1 / TR[2];
	real lzl = LL[2], lzr = LR[2], tzl = TL[2], tzr = TR[2];
	real l = tzl - lzl, r = tzr - lzr;
	real ratio;
	int rev = l < 0 ? -1 : 1;//-1:occlude left 1:occlude right

	if (l*r >= 0) {
		if (l == 0) IntersectResult[2] = r > 0 ? -1 : 1;
		else if (r == 0) IntersectResult[2] = l > 0 ? -1 : 1;
		else
			IntersectResult[2] = rev;
		return 0;
	}
	l = fabsf(l);
	r = fabsf(r);
	ratio = l / (l + r);

	IntersectResult[2] = tnsLinearInterpolate(lzl, lzr, ratio);
	lanpr_GetInterpolatePoint2d(LL, LR, ratio, IntersectResult);

	return rev;
}
LANPR_RenderVert* lanpr_FindSharedVertex(LANPR_RenderLine* rl, LANPR_RenderTriangle* rt) {
	if (rl->L == rt->V[0] || rl->L == rt->V[1] || rl->L == rt->V[2]) return rl->L;
	elif(rl->R == rt->V[0] || rl->R == rt->V[1] || rl->R == rt->V[2]) return rl->R;
	else return 0;
}
void lanpr_FindEdgeNoVertex(LANPR_RenderTriangle* rt, LANPR_RenderVert* rv, tnsVector3d L, tnsVector3d R) {
	if (rt->V[0] == rv) {
		tMatVectorCopy3d(rt->V[1]->FrameBufferCoord, L);
		tMatVectorCopy3d(rt->V[2]->FrameBufferCoord, R);
	}elif(rt->V[1] == rv) {
		tMatVectorCopy3d(rt->V[2]->FrameBufferCoord, L);
		tMatVectorCopy3d(rt->V[0]->FrameBufferCoord, R);
	}elif(rt->V[2] == rv) {
		tMatVectorCopy3d(rt->V[0]->FrameBufferCoord, L);
		tMatVectorCopy3d(rt->V[1]->FrameBufferCoord, R);
	}
}
LANPR_RenderLine* lanpr_AnotherEdge(LANPR_RenderTriangle* rt, LANPR_RenderVert* rv) {
	if (rt->V[0] == rv) {
		return rt->RL[1];
	}elif(rt->V[1] == rv) {
		return rt->RL[2];
	}elif(rt->V[2] == rv) {
		return rt->RL[0];
	}
}

int lanpr_ShareEdge(LANPR_RenderTriangle* rt, LANPR_RenderVert* rv, LANPR_RenderLine* rl) {
	LANPR_RenderVert* another = rv == rl->L ? rl->R : rl->L;

	if (rt->V[0] == rv) {
		if (another == rt->V[1] || another == rt->V[2]) return 1;
		return 0;
	}elif(rt->V[1] == rv) {
		if (another == rt->V[0] || another == rt->V[2]) return 1;
		return 0;
	}elif(rt->V[2] == rv) {
		if (another == rt->V[0] || another == rt->V[1]) return 1;
		return 0;
	}
}
int lanpr_ShareEdgeDirect(LANPR_RenderTriangle* rt, LANPR_RenderLine* rl) {
	if (rt->RL[0] == rl || rt->RL[1] == rl || rt->RL[2] == rl)
		return 1;
	return 0;
}
int lanpr_TriangleLineImageSpaceIntersectTestOnly(LANPR_RenderTriangle* rt, LANPR_RenderLine* rl, double* From, double* To) {
	double dl, dr;
	double ratio;
	double is[3];
	int order[3];
	int LCross, RCross;
	int a, b, c;
	int ret;
	int noCross = 0;
	tnsVector3d TL, TR, LL, LR;
	tnsVector3d IntersectResult;
	LANPR_RenderVert* Share;
	int StL = 0, StR = 0;
	int OccludeSide;

	double
		*LFBC = rl->L->FrameBufferCoord,
		*RFBC = rl->R->FrameBufferCoord,
		*FBC0 = rt->V[0]->FrameBufferCoord,
		*FBC1 = rt->V[1]->FrameBufferCoord,
		*FBC2 = rt->V[2]->FrameBufferCoord;

	//bound box.
	if (TNS_MIN3(FBC0[2], FBC1[2], FBC2[2]) > TNS_MAX2(LFBC[2], RFBC[2])) return 0;
	if (TNS_MAX3(FBC0[0], FBC1[0], FBC2[0]) < TNS_MIN2(LFBC[0], RFBC[0])) return 0;
	if (TNS_MIN3(FBC0[0], FBC1[0], FBC2[0]) > TNS_MAX2(LFBC[0], RFBC[0])) return 0;
	if (TNS_MAX3(FBC0[1], FBC1[1], FBC2[1]) < TNS_MIN2(LFBC[1], RFBC[1])) return 0;
	if (TNS_MIN3(FBC0[1], FBC1[1], FBC2[1]) > TNS_MAX2(LFBC[1], RFBC[1])) return 0;

	if (Share = lanpr_FindSharedVertex(rl, rt)) {
		tnsVector3d CL, CR;
		double r;
		int status;
		double r2;

		//if (rl->IgnoreConnectedFace/* && lanpr_ShareEdge(rt, Share, rl)*/) 
		//return 0;

		lanpr_FindEdgeNoVertex(rt, Share, CL, CR);
		status = lanpr_LineIntersectTest2d(LFBC, RFBC, CL, CR, &r);

		//LL[2] = 1 / tnsLinearItp(1 / LFBC[2], 1 / RFBC[2], r);
		LL[0] = tnsLinearItp(LFBC[0], RFBC[0], r);
		LL[1] = tnsLinearItp(LFBC[1], RFBC[1], r);
		LL[2] = tnsLinearItp(LFBC[2], RFBC[2], r);

		r2 = lanpr_GetLinearRatio(CL, CR, LL);
		//LR[2] = 1 / tnsLinearItp(1 / CL[2], 1 / CR[2], r2);
		LR[0] = tnsLinearItp(CL[0], CR[0], r2);
		LR[1] = tnsLinearItp(CL[1], CR[1], r2);
		LR[2] = tnsLinearItp(CL[2], CR[2], r2);


		if (LL[2] <= (LR[2] + 0.000000001)) return 0;

		StL = lanpr_PointInsideTrianglef(LFBC, FBC0, FBC1, FBC2);
		StR = lanpr_PointInsideTrianglef(RFBC, FBC0, FBC1, FBC2);

		if ((StL && Share == rl->R) ||
			(StR && Share == rl->L)) {
			*From = 0;
			*To = 1;
			return 1;
		}

		if (!status) return 0;

		if (rl->L == Share) {
			*From = 0;
			*To = r;
		}
		else {
			*From = r;
			*To = 1;
		}
		return 1;

	}

	a = lanpr_LineIntersectTest2d(LFBC, RFBC, FBC0, FBC1, &is[0]);
	b = lanpr_LineIntersectTest2d(LFBC, RFBC, FBC1, FBC2, &is[1]);
	c = lanpr_LineIntersectTest2d(LFBC, RFBC, FBC2, FBC0, &is[2]);

	if (!a && !b && !c) {
		if (!(StL = lanpr_PointTriangleRelation(LFBC, FBC0, FBC1, FBC2)) &&
			!(StR = lanpr_PointTriangleRelation(RFBC, FBC0, FBC1, FBC2))) {
			return 0;//not occluding
		}
	}

	StL = lanpr_PointTriangleRelation(LFBC, FBC0, FBC1, FBC2);
	StR = lanpr_PointTriangleRelation(RFBC, FBC0, FBC1, FBC2);

	INTERSECT_SORT_MIN_TO_MAX_3(is[0], is[1], is[2], order);

	if (StL) {
		INTERSECT_JUST_SMALLER(is, order, DBL_TRIANGLE_LIM, LCross);
		INTERSECT_JUST_GREATER(is, order, -DBL_TRIANGLE_LIM, RCross);
		//if (is[LCross]>=0 || is[RCross] >= 1) return 0;
	}elif(StR) {
		INTERSECT_JUST_SMALLER(is, order, 1.0f + DBL_TRIANGLE_LIM, LCross);
		INTERSECT_JUST_GREATER(is, order, 1.0f - DBL_TRIANGLE_LIM, RCross);
		//if (is[LCross] <= 0 || is[RCross] <= 1) return 0;
	}
	else {
		if (a) {
			if (b) {
				LCross = is[0] < is[1] ? 0 : 1;
				RCross = is[0] < is[1] ? 1 : 0;
			}
			else {
				LCross = is[0] < is[2] ? 0 : 2;
				RCross = is[0] < is[2] ? 2 : 0;
			}
		}elif(c) {
			LCross = is[1] < is[2] ? 1 : 2;
			RCross = is[1] < is[2] ? 2 : 1;
		}
		else {
			return 0;
		}
		//if (rl->IgnoreConnectedFace/* && lanpr_ShareEdge(rt, Share, rl)*/)
		//	return 0;
		if (TNS_MAX3(FBC0[2], FBC1[2], FBC2[2]) <(TNS_MIN2(LFBC[2], RFBC[2]) - 0.000001)) {
			*From = is[LCross];
			*To = is[RCross];
			TNS_CLAMP((*From), 0, 1);
			TNS_CLAMP((*To), 0, 1);
			return 1;
		}
	}

	LL[2] = lanpr_GetLineZ(LFBC, RFBC, is[LCross]);
	LR[2] = lanpr_GetLineZ(LFBC, RFBC, is[RCross]);
	lanpr_GetInterpolatePoint2d(LFBC, RFBC, is[LCross], LL);
	lanpr_GetInterpolatePoint2d(LFBC, RFBC, is[RCross], LR);

	TL[2] = lanpr_GetLineZPoint(rt->V[LCross]->FrameBufferCoord, rt->V[(LCross > 1 ? 0 : (LCross + 1))]->FrameBufferCoord, LL);
	TR[2] = lanpr_GetLineZPoint(rt->V[RCross]->FrameBufferCoord, rt->V[(RCross > 1 ? 0 : (RCross + 1))]->FrameBufferCoord, LR);
	tMatVectorCopy2d(LL, TL);
	tMatVectorCopy2d(LR, TR);

	if (OccludeSide = tRdtGetZIntersectPoint(TL, TR, LL, LR, IntersectResult)) {
		real r = lanpr_GetLinearRatio(LFBC, RFBC, IntersectResult);
		if (OccludeSide > 0) {
			if (r > 1 /*|| r < 0*/) return 0;
			*From = TNS_MAX2(r, 0);
			*To = TNS_MIN2(is[RCross], 1);
		}
		else {
			if (r < 0 /*|| r > 1*/) return 0;
			*From = TNS_MAX2(is[LCross], 0);
			*To = TNS_MIN2(r, 1);
		}
		//*From = TNS_MAX2(TNS_MAX2(r, is[LCross]), 0);
		//*To = TNS_MIN2(r, TNS_MIN2(is[RCross], 1));
	}elif(IntersectResult[2]<0) {
		if ((!StL) && (!StR) && (a + b + c < 2) || is[LCross]>is[RCross]) return 0;
		*From = is[LCross];
		*To = is[RCross];
	}
	else return 0;

	TNS_CLAMP((*From), 0, 1);
	TNS_CLAMP((*To), 0, 1);

	//if ((TNS_FLOAT_CLOSE_ENOUGH(*From, 0) && TNS_FLOAT_CLOSE_ENOUGH(*To, 1)) ||
	//	(TNS_FLOAT_CLOSE_ENOUGH(*To, 0) && TNS_FLOAT_CLOSE_ENOUGH(*From, 1)) ||
	//	TNS_FLOAT_CLOSE_ENOUGH(*From, *To)) return 0;

	return 1;
}
int lanpr_TriangleLineImageSpaceIntersectTestOnlyV2(LANPR_RenderTriangle* rt, LANPR_RenderLine* rl, Camera* cam, tnsMatrix44d vp, double* From, double* To) {
	double dl, dr;
	double ratio;
	double is[3] = { 0 };
	int order[3];
	int LCross = -1, RCross = -1;
	int a, b, c;
	int ret;
	int noCross = 0;
	tnsVector3d TL, TR, LL, LR;
	tnsVector3d IntersectResult;
	LANPR_RenderVert* Share;
	int StL = 0, StR = 0, In;
	int OccludeSide;

	tnsVector3d LV;
	tnsVector3d RV;
	real* CV = &cam->RenderViewDir;
	real DotL, DotR, DotLA, DotRA;
	real DotF;
	LANPR_RenderVert* Result, *rv;
	tnsVector3d GLocation, Trans;
	real Cut = -1;
	int NextCut, NextCut1;
	int status;


	double
		*LFBC = rl->L->FrameBufferCoord,
		*RFBC = rl->R->FrameBufferCoord,
		*FBC0 = rt->V[0]->FrameBufferCoord,
		*FBC1 = rt->V[1]->FrameBufferCoord,
		*FBC2 = rt->V[2]->FrameBufferCoord;

	//printf("%f %f %f   %f %f\n", FBC0[2], FBC1[2], FBC2[2], LFBC[2], RFBC[2]);

	//bound box.
	if (TNS_MIN3(FBC0[2], FBC1[2], FBC2[2]) > TNS_MAX2(LFBC[2], RFBC[2]))
		return 0;
	if (TNS_MAX3(FBC0[0], FBC1[0], FBC2[0]) < TNS_MIN2(LFBC[0], RFBC[0])) return 0;
	if (TNS_MIN3(FBC0[0], FBC1[0], FBC2[0]) > TNS_MAX2(LFBC[0], RFBC[0])) return 0;
	if (TNS_MAX3(FBC0[1], FBC1[1], FBC2[1]) < TNS_MIN2(LFBC[1], RFBC[1])) return 0;
	if (TNS_MIN3(FBC0[1], FBC1[1], FBC2[1]) > TNS_MAX2(LFBC[1], RFBC[1])) return 0;

	if (lanpr_ShareEdgeDirect(rt, rl))
		return 0;

	a = lanpr_LineIntersectTest2d(LFBC, RFBC, FBC0, FBC1, &is[0]);
	b = lanpr_LineIntersectTest2d(LFBC, RFBC, FBC1, FBC2, &is[1]);
	c = lanpr_LineIntersectTest2d(LFBC, RFBC, FBC2, FBC0, &is[2]);

	INTERSECT_SORT_MIN_TO_MAX_3(is[0], is[1], is[2], order);

	tMatVectorMinus3d(LV, rl->L->GLocation, rt->V[0]->GLocation);
	tMatVectorMinus3d(RV, rl->R->GLocation, rt->V[0]->GLocation);

	if (cam->type == CAM_PERSP) tMatVectorMinus3d(CV, cam->Base.GLocation, rt->V[0]->GLocation);

	DotL = tMatDot3d(LV, rt->GN, 0);
	DotR = tMatDot3d(RV, rt->GN, 0);
	DotF = tMatDot3d(CV, rt->GN, 0);

	if (!DotF) return 0;

	//if ((Share = lanpr_FindSharedVertex(rl, rt)) ||
	//	(!rl->TL && ((Share = lanpr_ShareEdgeDirect(rt, rl->L->IntersectingLine)?rl->L:
	//	(lanpr_ShareEdgeDirect(rt, rl->R->IntersectingLine)? rl->R:0))))) {

	//	tnsVector3d CL, CR;
	//	double r;
	//	double r2;
	//	LANPR_RenderVert* another = Share == rl->L ? rl->R : rl->L;
	//	int in,index;
	//	real cut;

	//	if (lanpr_ShareEdgeDirect(rt, rl))
	//		return 0;

	//	in = lanpr_PointInsideTrianglef(another->FrameBufferCoord, rt->V[0]->FrameBufferCoord, rt->V[1]->FrameBufferCoord, rt->V[2]->FrameBufferCoord);

	//	if (DotR*DotF + DotL*DotF >= 0) return 0;

	//	if (!rl->TL) {
	//		if (in) {
	//			*From = 0;
	//			*To = 1;
	//			return 1;
	//		}else {
	//			if (another->IntersectingLine == rt->RL[0]) cut = is[0];
	//			elif(another->IntersectingLine == rt->RL[1]) cut = is[1];
	//			elif(another->IntersectingLine == rt->RL[2]) cut = is[2];
	//			if (Share == rl->L) {
	//				*From = 0;
	//				INTERSECT_JUST_GREATER(is, order, 0, index);
	//				if (!TNS_ABC(index)) return 0;
	//				if (is[index] < DBL_EPSILON) return 0;
	//				*To = is[index];
	//				//TNS_CLAMP(*From, 0, 1);
	//				//TNS_CLAMP(*To, 0, 1);
	//				return 1;
	//			}else {
	//				INTERSECT_JUST_SMALLER(is, order, 1, index);
	//				if (!TNS_ABC(index)) return 0;
	//				if (is[index] < DBL_EPSILON) return 0;
	//				*From = is[index];
	//				*To = 1;
	//				//TNS_CLAMP(*From, 0, 1);
	//				//TNS_CLAMP(*To, 0, 1);
	//				return 1;
	//			}
	//		}
	//	}

	//	if (Share == rl->L) {
	//		if (in) {
	//			*From = 0;
	//			*To = 1;
	//			return 1;
	//		}else {
	//			cut = TNS_MAX3(is[0], is[1], is[2]);
	//			if (!TNS_MAX3_INDEX_ABC(is[0], is[1], is[2])) return 0;
	//			if (cut <= 1 && cut > 0.000001) {
	//				*From = 0;
	//				*To = cut;
	//				return 1;
	//			}
	//			return 0;
	//		}
	//	}else {
	//		if (in) {
	//			*From = 0;
	//			*To = 1;
	//			return 1;
	//		}
	//		else {
	//			cut = TNS_MIN3(is[0], is[1], is[2]);
	//			if (!TNS_MIN3_INDEX_ABC(is[0], is[1], is[2])) return 0;
	//			if (cut <= 0.99999 && cut > 0.000001) {
	//				*From = cut;
	//				*To = 1;
	//				return 1;
	//			}
	//			return 0;
	//		}
	//	}

	//}

	if (!a && !b && !c) {
		if (!(StL = lanpr_PointTriangleRelation(LFBC, FBC0, FBC1, FBC2)) &&
			!(StR = lanpr_PointTriangleRelation(RFBC, FBC0, FBC1, FBC2))) {
			return 0;//not occluding
		}
	}

	StL = lanpr_PointTriangleRelation(LFBC, FBC0, FBC1, FBC2);
	StR = lanpr_PointTriangleRelation(RFBC, FBC0, FBC1, FBC2);


	//for (rv = rt->IntersectingVerts.pFirst; rv; rv = rv->Item.pNext) {
	//	if (rv->IntersectWith == rt && rv->IntersectingLine == rl) {
	//		Cut = tMatGetLinearRatio(rl->L->FrameBufferCoord[0], rl->R->FrameBufferCoord[0], rv->FrameBufferCoord[0]);
	//		break;
	//	}
	//}


	DotLA = fabs(DotL); if (DotLA < DBL_EPSILON) { DotLA = 0; DotL = 0; }
	DotRA = fabs(DotR); if (DotRA < DBL_EPSILON) { DotRA = 0; DotR = 0; }
	if (DotL - DotR == 0) Cut = 100000;
	else if (DotL*DotR <= 0) {
		Cut = DotLA / fabs(DotL - DotR);
	}
	else {
		Cut = fabs(DotR + DotL) / fabs(DotL - DotR);
		Cut = DotRA > DotLA ? 1 - Cut : Cut;
	}

	if (cam->type == CAM_PERSP) {
		tnsLinearInterpolate3dv(rl->L->GLocation, rl->R->GLocation, Cut, GLocation);
		tMatApplyTransform44d(Trans, vp, GLocation);
		tMatVectorMultiSelf3d(Trans, (1 / Trans[3])/**HeightMultiply/2*/);
	}
	else {
		tnsLinearInterpolate3dv(rl->L->FrameBufferCoord, rl->R->FrameBufferCoord, Cut, Trans);
		//tMatApplyTransform44d(Trans, vp, GLocation);
	}

	//Trans[2] = tMatDist3dv(GLocation, cam->Base.GLocation);
	//Trans[2] = cam->clipsta*cam->clipend / (cam->clipend - fabs(Trans[2]) * (cam->clipend - cam->clipsta));


	Cut = tMatGetLinearRatio(rl->L->FrameBufferCoord[0], rl->R->FrameBufferCoord[0], Trans[0]);

	In = lanpr_PointInsideTrianglef(Trans, rt->V[0]->FrameBufferCoord, rt->V[1]->FrameBufferCoord, rt->V[2]->FrameBufferCoord);


	if (StL == 2) {
		if (StR == 2) {
			INTERSECT_JUST_SMALLER(is, order, DBL_TRIANGLE_LIM, LCross);
			INTERSECT_JUST_GREATER(is, order, 1 - DBL_TRIANGLE_LIM, RCross);
		}elif(StR == 1) {
			INTERSECT_JUST_SMALLER(is, order, DBL_TRIANGLE_LIM, LCross);
			INTERSECT_JUST_GREATER(is, order, 1 - DBL_TRIANGLE_LIM, RCross);
		}elif(StR == 0) {
			INTERSECT_JUST_SMALLER(is, order, DBL_TRIANGLE_LIM, LCross);
			INTERSECT_JUST_GREATER(is, order, 0, RCross);
		}
	}elif(StL == 1) {
		if (StR == 2) {
			INTERSECT_JUST_SMALLER(is, order, DBL_TRIANGLE_LIM, LCross);
			INTERSECT_JUST_GREATER(is, order, 1 - DBL_TRIANGLE_LIM, RCross);
		}elif(StR == 1) {
			INTERSECT_JUST_SMALLER(is, order, DBL_TRIANGLE_LIM, LCross);
			INTERSECT_JUST_GREATER(is, order, 1 - DBL_TRIANGLE_LIM, RCross);
		}elif(StR == 0) {
			INTERSECT_JUST_GREATER(is, order, DBL_TRIANGLE_LIM, RCross);
			if (TNS_ABC(RCross) && is[RCross] > (DBL_TRIANGLE_LIM)) {
				INTERSECT_JUST_SMALLER(is, order, DBL_TRIANGLE_LIM, LCross);
			}
			else {
				INTERSECT_JUST_SMALLER(is, order, -DBL_TRIANGLE_LIM, LCross);
				INTERSECT_JUST_GREATER(is, order, -DBL_TRIANGLE_LIM, RCross);
			}
		}
	}elif(StL == 0) {
		if (StR == 2) {
			INTERSECT_JUST_SMALLER(is, order, 1 - DBL_TRIANGLE_LIM, LCross);
			INTERSECT_JUST_GREATER(is, order, 1 - DBL_TRIANGLE_LIM, RCross);
		}elif(StR == 1) {
			INTERSECT_JUST_SMALLER(is, order, 1 - DBL_TRIANGLE_LIM, LCross);
			if (TNS_ABC(LCross) && is[LCross] < (1 - DBL_TRIANGLE_LIM)) {
				INTERSECT_JUST_GREATER(is, order, 1 - DBL_TRIANGLE_LIM, RCross);
			}
			else {
				INTERSECT_JUST_SMALLER(is, order, 1 + DBL_TRIANGLE_LIM, LCross);
				INTERSECT_JUST_GREATER(is, order, 1 + DBL_TRIANGLE_LIM, RCross);
			}
		}elif(StR == 0) {
			INTERSECT_JUST_GREATER(is, order, 0, LCross);
			if (TNS_ABC(LCross) && is[LCross] > 0) {
				INTERSECT_JUST_GREATER(is, order, is[LCross], RCross);
			}
			else {
				INTERSECT_JUST_GREATER(is, order, is[LCross], LCross);
				INTERSECT_JUST_GREATER(is, order, is[LCross], RCross);
			}
		}
	}


	real LF = DotL*DotF, RF = DotR*DotF;
	//int CrossCount = a + b + c;
	//if (CrossCount == 2) {
	//	INTERSECT_JUST_GREATER(is, order, 0, LCross);
	//	if (!TNS_ABC(LCross)) INTERSECT_JUST_GREATER(is, order, is[LCross], LCross);
	//	INTERSECT_JUST_GREATER(is, order, is[LCross], RCross);
	//}elif(CrossCount == 1 || StL+StR==1) {
	//	if (StL) {
	//		INTERSECT_JUST_GREATER(is, order, DBL_TRIANGLE_LIM, RCross);
	//		INTERSECT_JUST_SMALLER(is, order, is[RCross], LCross);
	//	}elif(StR) {
	//		INTERSECT_JUST_SMALLER(is, order, 1 - DBL_TRIANGLE_LIM, LCross);
	//		INTERSECT_JUST_GREATER(is, order, is[LCross], RCross);
	//	}
	//}elif(CrossCount == 0) {
	//	INTERSECT_JUST_SMALLER(is, order, 0, LCross);
	//	INTERSECT_JUST_GREATER(is, order, 1, RCross);
	//}

	if (LF <= 0 && RF <= 0 && (DotL || DotR)) {

		*From = TNS_MAX2(0, is[LCross]);
		*To = TNS_MIN2(1, is[RCross]);
		if (*From >= *To) return 0;
		return 1;
	}elif(LF >= 0 && RF <= 0 && (DotL || DotR)) {
		*From = TNS_MAX2(Cut, is[LCross]);
		*To = TNS_MIN2(1, is[RCross]);
		if (*From >= *To) return 0;
		return 1;
	}elif(LF <= 0 && RF >= 0 && (DotL || DotR)) {
		*From = TNS_MAX2(0, is[LCross]);
		*To = TNS_MIN2(Cut, is[RCross]);
		if (*From >= *To) return 0;
		return 1;
	}
	else
		return 0;
	return 1;
}

LANPR_RenderLine* lanpr_TriangleShareEdge(LANPR_RenderTriangle* l, LANPR_RenderTriangle* r) {
	if (l->RL[0] == r->RL[0]) return r->RL[0];
	if (l->RL[0] == r->RL[1]) return r->RL[1];
	if (l->RL[0] == r->RL[2]) return r->RL[2];
	if (l->RL[1] == r->RL[0]) return r->RL[0];
	if (l->RL[1] == r->RL[1]) return r->RL[1];
	if (l->RL[1] == r->RL[2]) return r->RL[2];
	if (l->RL[2] == r->RL[0]) return r->RL[0];
	if (l->RL[2] == r->RL[1]) return r->RL[1];
	if (l->RL[2] == r->RL[2]) return r->RL[2];
	return 0;
}
LANPR_RenderVert* lanpr_TriangleSharePoint(LANPR_RenderTriangle* l, LANPR_RenderTriangle* r) {
	if (l->V[0] == r->V[0]) return r->V[0];
	if (l->V[0] == r->V[1]) return r->V[1];
	if (l->V[0] == r->V[2]) return r->V[2];
	if (l->V[1] == r->V[0]) return r->V[0];
	if (l->V[1] == r->V[1]) return r->V[1];
	if (l->V[1] == r->V[2]) return r->V[2];
	if (l->V[2] == r->V[0]) return r->V[0];
	if (l->V[2] == r->V[1]) return r->V[1];
	if (l->V[2] == r->V[2]) return r->V[2];
	return 0;
}



// lanpr_AssociateLineWithTile


LANPR_RenderVert* lanpr_TriangleLineIntersectionTest(LANPR_RenderLine* rl, LANPR_RenderTriangle* rt, LANPR_RenderTriangle* Testing, LANPR_RenderVert* Last) {
	tnsVector3d LV;
	tnsVector3d RV;
	real DotL, DotR;
	LANPR_RenderVert* Result, *rv;
	tnsVector3d GLocation;
	LANPR_RenderVert* L = rl->L, *R = rl->R;
	int result;

	int i;

	for (rv = Testing->IntersectingVerts.pFirst; rv; rv = rv->Item.pNext) {
		if (rv->IntersectWith == rt && rv->IntersectingLine == rl)
			return rv;
	}


	tMatVectorMinus3d(LV, L->GLocation, Testing->V[0]->GLocation);
	tMatVectorMinus3d(RV, R->GLocation, Testing->V[0]->GLocation);

	DotL = tMatDot3d(LV, Testing->GN, 0);
	DotR = tMatDot3d(RV, Testing->GN, 0);

	if (DotL*DotR > 0 || (!DotL && !DotR))
		return 0;

	DotL = fabs(DotL);
	DotR = fabs(DotR);

	tnsLinearInterpolate3dv(L->GLocation, R->GLocation, DotL / (DotL + DotR), GLocation);

	if (Last && TNS_DOUBLE_CLOSE_ENOUGH(Last->GLocation[0], GLocation[0])
		&& TNS_DOUBLE_CLOSE_ENOUGH(Last->GLocation[1], GLocation[1])
		&& TNS_DOUBLE_CLOSE_ENOUGH(Last->GLocation[2], GLocation[2])) {

		Last->IntersectintLine2 = rl;
		return 0;
	}

	if (!(result = lanpr_PointInsideTriangle3de(GLocation, Testing->V[0]->GLocation, Testing->V[1]->GLocation, Testing->V[2]->GLocation)))
		return 0;
	/*elif(result < 0) {
	return 0;
	}*/



	Result = memAquireOnly(sizeof(LANPR_RenderVert));

	if (DotL > 0 || DotR<0) Result->Positive = 1; else Result->Positive = 0;

	//Result->IntersectingOnFace = Testing;
	Result->EdgeUsed = 1;
	//Result->IntersectL = L;
	Result->V = (void*)R; //Caution!
				          //Result->IntersectWith = rt;
	tMatVectorCopy3d(GLocation, Result->GLocation);

	lstAppendItem(&Testing->IntersectingVerts, Result);

	return Result;
}
LANPR_RenderLine* lanpr_TriangleGenerateIntersectionLineOnly(LANPR_RenderBuffer* rb, LANPR_RenderTriangle* rt, LANPR_RenderTriangle* Testing) {
	LANPR_RenderVert* L = 0, *R = 0;
	LANPR_RenderVert** Next = &L;
	LANPR_RenderLine* Result;
	LANPR_RenderVert* E0T = 0;
	LANPR_RenderVert* E1T = 0;
	LANPR_RenderVert* E2T = 0;
	LANPR_RenderVert* TE0 = 0;
	LANPR_RenderVert* TE1 = 0;
	LANPR_RenderVert* TE2 = 0;
	LANPR_RenderBuffer* fb = rb;
	tnsVector3d* cl = rb->Scene->camera->GLocation;
	real clipend = ((Camera*)rb->Scene->camera)->clipend;
	real clipsta = ((Camera*)rb->Scene->camera)->clipsta;
	LANPR_RenderVert* Share = lanpr_TriangleSharePoint(Testing, rt);

	if (Share) {
		LANPR_RenderVert* NewShare;
		LANPR_RenderLine* rl = lanpr_AnotherEdge(rt, Share);

		L = NewShare = memStaticAquire(&rb->RenderDataPool, (sizeof(LANPR_RenderVert)));

		NewShare->Positive = 1;
		NewShare->EdgeUsed = 1;
		//NewShare->IntersectL = L;
		NewShare->V = R; //Caution!
						 //Result->IntersectWith = rt;
		tMatVectorCopy3d(Share->GLocation, NewShare->GLocation);

		R = lanpr_TriangleLineIntersectionTest(rl, rt, Testing, 0);

		if (!R) {
			rl = lanpr_AnotherEdge(Testing, Share);
			R = lanpr_TriangleLineIntersectionTest(rl, Testing, rt, 0);
			if (!R) return 0;
			lstAppendItem(&Testing->IntersectingVerts, NewShare);
		}
		else {
			lstAppendItem(&rt->IntersectingVerts, NewShare);
		}

	}
	else {
		E0T = lanpr_TriangleLineIntersectionTest(rt->RL[0], rt, Testing, 0); if (E0T && (!(*Next))) { (*Next) = E0T; (*Next)->IntersectingLine = rt->RL[0];  Next = &R; }
		E1T = lanpr_TriangleLineIntersectionTest(rt->RL[1], rt, Testing, L); if (E1T && (!(*Next))) { (*Next) = E1T; (*Next)->IntersectingLine = rt->RL[1];  Next = &R; }
		if (!(*Next)) E2T = lanpr_TriangleLineIntersectionTest(rt->RL[2], rt, Testing, L); if (E2T && (!(*Next))) { (*Next) = E2T; (*Next)->IntersectingLine = rt->RL[2];  Next = &R; }

		if (!(*Next))TE0 = lanpr_TriangleLineIntersectionTest(Testing->RL[0], Testing, rt, L); if (TE0 && (!(*Next))) { (*Next) = TE0; (*Next)->IntersectingLine = Testing->RL[0]; Next = &R; }
		if (!(*Next))TE1 = lanpr_TriangleLineIntersectionTest(Testing->RL[1], Testing, rt, L); if (TE1 && (!(*Next))) { (*Next) = TE1; (*Next)->IntersectingLine = Testing->RL[1]; Next = &R; }
		if (!(*Next))TE2 = lanpr_TriangleLineIntersectionTest(Testing->RL[2], Testing, rt, L); if (TE2 && (!(*Next))) { (*Next) = TE2; (*Next)->IntersectingLine = Testing->RL[2]; Next = &R; }

		if (!(*Next)) return 0;
	}
	tMatApplyTransform44d(L->FrameBufferCoord, fb->ViewProjection, L->GLocation);
	tMatApplyTransform44d(R->FrameBufferCoord, fb->ViewProjection, R->GLocation);
	tMatVectorMultiSelf3d(L->FrameBufferCoord, (1 / L->FrameBufferCoord[3])/**HeightMultiply/2*/);
	tMatVectorMultiSelf3d(R->FrameBufferCoord, (1 / R->FrameBufferCoord[3])/**HeightMultiply/2*/);

	//L->FrameBufferCoord[2] = tMatDist3dv(L->GLocation, cl);
	L->FrameBufferCoord[2] = clipsta*clipend / (clipend - fabs(L->FrameBufferCoord[2]) * (clipend - clipsta));
	//if (L->FrameBufferCoord[3] < 0) {
	//	L->FrameBufferCoord[1] *= -1;
	//	L->FrameBufferCoord[0] *= -1;
	//	L->FrameBufferCoord[2] *= -1;
	//}

	//R->FrameBufferCoord[2] = tMatDist3dv(R->GLocation, cl);
	R->FrameBufferCoord[2] = clipsta*clipend / (clipend - fabs(R->FrameBufferCoord[2]) * (clipend - clipsta));
	//if (R->FrameBufferCoord[3] < 0) {
	//	R->FrameBufferCoord[1] *= -1;
	//	R->FrameBufferCoord[0] *= -1;
	//	R->FrameBufferCoord[2] *= -1;
	//}



	/*if (L->FrameBufferCoord[3] < 0) {
	L->FrameBufferCoord[2] *= -1;
	L->FrameBufferCoord[1] *= -1;
	L->FrameBufferCoord[0] *= -1;
	}
	L->FrameBufferCoord[2] /= L->FrameBufferCoord[3];
	L->FrameBufferCoord[3] = L->FrameBufferCoord[2];
	L->FrameBufferCoord[2] = clipsta*clipend / (clipend - L->FrameBufferCoord[2] * (clipend - clipsta));

	if (R->FrameBufferCoord[3] < 0) {
	R->FrameBufferCoord[2] *= -1;
	R->FrameBufferCoord[1] *= -1;
	R->FrameBufferCoord[0] *= -1;
	}
	R->FrameBufferCoord[2] /= R->FrameBufferCoord[3];
	R->FrameBufferCoord[3] = R->FrameBufferCoord[2];
	R->FrameBufferCoord[2] = clipsta*clipend / (clipend - R->FrameBufferCoord[2] * (clipend - clipsta));
	*/

	L->IntersectWith = rt;
	R->IntersectWith = Testing;

	///*((1 / rl->L->FrameBufferCoord[3])*rb->FrameBuffer->H / 2)

	Result = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLine));
	Result->L = L;
	Result->R = R;
	LANPR_RenderLineSegment* rls = memStaticAquire(&rb->RenderDataPool, sizeof(LANPR_RenderLineSegment));
	lstAppendItem(&Result->Segments, rls);
	lstAppendItem(&rb->AllRenderLines, Result);
	lstAppendPointerStatic(&rb->IntersectionLines, &rb->RenderDataPool, Result);

	tnsglobal_TriangleIntersectionCount++;

	//rb->IntersectionCount++;

	return Result;
}
int lanpr_TriangleCalculateIntersectionsInTile(LANPR_RenderBuffer* rb, LANPR_RenderTriangle* rt, LANPR_BoundingArea* ba) {
	tnsVector3d n, c = { 0 };
	tnsVector3d TL, TR;
	LANPR_RenderTriangle* TestingTriangle;
	LANPR_RenderLine* TestingLine;
	LANPR_RenderLine* Result = 0;
	LANPR_RenderVert* rv;
	nListItemPointer* lip, *NextLip;
	real l, r;
	int a = 0;

	real
		*FBC0 = rt->V[0]->FrameBufferCoord,
		*FBC1 = rt->V[1]->FrameBufferCoord,
		*FBC2 = rt->V[2]->FrameBufferCoord;

	for (lip = ba->AssociatedTriangles.pFirst; lip; lip = NextLip) {
		NextLip = lip->pNext;
		TestingTriangle = lip->p;
		if (TestingTriangle == rt || TestingTriangle->Testing == rt || lanpr_TriangleShareEdge(rt, TestingTriangle))
			continue;
		TestingTriangle->Testing = rt;
		real
			*RFBC0 = TestingTriangle->V[0]->FrameBufferCoord,
			*RFBC1 = TestingTriangle->V[1]->FrameBufferCoord,
			*RFBC2 = TestingTriangle->V[2]->FrameBufferCoord;

		if (TNS_MIN3(FBC0[2], FBC1[2], FBC2[2]) > TNS_MAX3(RFBC0[2], RFBC1[2], RFBC2[2])) continue;
		if (TNS_MAX3(FBC0[2], FBC1[2], FBC2[2]) < TNS_MIN3(RFBC0[2], RFBC1[2], RFBC2[2])) continue;
		if (TNS_MIN3(FBC0[0], FBC1[0], FBC2[0]) > TNS_MAX3(RFBC0[0], RFBC1[0], RFBC2[0])) continue;
		if (TNS_MAX3(FBC0[0], FBC1[0], FBC2[0]) < TNS_MIN3(RFBC0[0], RFBC1[0], RFBC2[0])) continue;
		if (TNS_MIN3(FBC0[1], FBC1[1], FBC2[1]) > TNS_MAX3(RFBC0[1], RFBC1[1], RFBC2[1])) continue;
		if (TNS_MAX3(FBC0[1], FBC1[1], FBC2[1]) < TNS_MIN3(RFBC0[1], RFBC1[1], RFBC2[1])) continue;


		Result = lanpr_TriangleGenerateIntersectionLineOnly(rb, rt, TestingTriangle);
	}

}


int lanpr_LineCrossesFrame(tnsVector2d L, tnsVector2d R) {
	real vx, vy;
	tnsVector4d Converted;
	real c1, c;

	if (-1 > TNS_MAX2(L[0], R[0])) return 0;
	if (1 < TNS_MIN2(L[0], R[0])) return 0;
	if (-1 > TNS_MAX2(L[1], R[1])) return 0;
	if (1 < TNS_MIN2(L[1], R[1])) return 0;

	vx = L[0] - R[0];
	vy = L[1] - R[1];

	c1 = vx * (-1 - L[1]) - vy * (-1 - L[0]);
	c = c1;

	c1 = vx * (-1 - L[1]) - vy * (1 - L[0]);
	if (c1*c <= 0)return 1;
	else c = c1;

	c1 = vx * (1 - L[1]) - vy * (-1 - L[0]);
	if (c1*c <= 0)return 1;
	else c = c1;

	c1 = vx * (1 - L[1]) - vy * (1 - L[0]);
	if (c1*c <= 0)return 1;
	else c = c1;

	return 0;
}

void lanpr_ComputeViewVector(LANPR_RenderBuffer* rb) {
	tnsVector3d Direction = { 0,0,-1 };
	tnsVector3d Trans;
	tnsMatrix44d inv;
	tMatInverse44d(inv, rb->Scene->camera->GlobalTransform);
	tMatApplyRotation43d(Trans, inv, Direction);
	tMatVectorCopy3d(Trans, rb->ViewVector);
	tMatVectorMultiSelf3d(Trans, -1);
	tMatVectorCopy3d(Trans, ((Camera*)rb->Scene->camera)->RenderViewDir);
}

void lanpr_ComputeSceneContours(LANPR_RenderBuffer* rb) {
	real* ViewVector = &rb->ViewVector;
	BMEdge* e;
	real Dot1 = 0, Dot2 = 0;
	real Result;
	tnsVector4d GNormal;
	int Add = 0;
	Camera* c = rb->Scene->camera->data;
	LANPR_RenderLine* rl;
	int ContourCount = 0;
	int CreaseCount = 0;
	int MaterialCount = 0;

	rb->OverallProgress = 20;
	rb->CalculationStatus = TNS_CALCULATION_CONTOUR;
	//nulThreadNotifyUsers("tns.render_buffer_list.calculation_status");

	if (c->type == CAM_ORTHO) {
		lanpr_ComputeViewVector(rb);
	}

	for (rl = rb->AllRenderLines.pFirst; rl; rl = rl->Item.pNext) {
		//if(rl->Testing)
		//if (!lanpr_LineCrossesFrame(rl->L->FrameBufferCoord, rl->R->FrameBufferCoord)) 
		//	continue;

		Add = 0; Dot1 = 0; Dot2 = 0;

		if (c->type == CAM_PERSP) {
			tMatVectorMinus3d(ViewVector, rl->L->GLocation, c->Base.GLocation);
		}

		if (rl->TL) Dot1 = tMatDot3d(ViewVector, rl->TL->GN, 0); else Add = 1;
		if (rl->TR) Dot2 = tMatDot3d(ViewVector, rl->TR->GN, 0); else Add = 1;

		if (!Add) {
			if ((Result = Dot1*Dot2) <= 0) Add = 1;
			elif(tMatDot3d(rl->TL->GN, rl->TR->GN, 0) < rb->CreaseCos) Add = 2;
			elif(rl->TL && rl->TR && rl->TL->F && rl->TR->F && rl->TL->F->MaterialID != rl->TR->F->MaterialID) Add = 3;
		}

		if (Add == 1) {
			lstAppendPointerStatic(&rb->Contours, &rb->RenderDataPool, rl);
			ContourCount++;
		}elif(Add == 2) {
			lstAppendPointerStatic(&rb->CreaseLines, &rb->RenderDataPool, rl);
			CreaseCount++;
		}elif(Add == 3) {
			lstAppendPointerStatic(&rb->MaterialLines, &rb->RenderDataPool, rl);
			MaterialCount++;
		}
		if (ContourCount >= 100000) {
			//tnsset_PlusRenderContourCount(rb, ContourCount);
			ContourCount = 0;
		}
		if (CreaseCount >= 100000) {
			//tnsset_PlusRenderCreaseCount(rb, CreaseCount);
			CreaseCount = 0;
		}
		if (MaterialCount >= 100000) {
			//tnsset_PlusRenderMaterialCount(rb, MaterialCount);
			MaterialCount = 0;
		}
	}
	//tnsset_PlusRenderContourCount(rb, ContourCount);
	//tnsset_PlusRenderCreaseCount(rb, CreaseCount);
	//tnsset_PlusRenderMaterialCount(rb, MaterialCount);
}


void lanpr_ClearRenderState(LANPR_RenderBuffer* rb) {
	rb->ContourCount = 0;
	rb->ContourManaged = 0;
	rb->IntersectionCount = 0;
	rb->IntersectionManaged = 0;
	rb->MaterialLineCount = 0;
	rb->MaterialManaged = 0;
	rb->CreaseCount = 0;
	rb->CreaseManaged = 0;
	rb->CalculationStatus = TNS_CALCULATION_IDLE;
}


/* ====================================== render control ======================================= */

void lanpr_DestroyRenderData(LANPR_RenderBuffer* rb) {
	LANPR_RenderElementLinkNode* reln;

	rb->ContourCount = 0;
	rb->ContourManaged = 0;
	rb->IntersectionCount = 0;
	rb->IntersectionManaged = 0;
	rb->MaterialLineCount = 0;
	rb->MaterialManaged = 0;
	rb->CreaseCount = 0;
	rb->CreaseManaged = 0;
	rb->CalculationStatus = TNS_CALCULATION_IDLE;

	lstEmptyDirect(&rb->Contours);
	lstEmptyDirect(&rb->IntersectionLines);
	lstEmptyDirect(&rb->CreaseLines);
	lstEmptyDirect(&rb->MaterialLines);
	lstEmptyDirect(&rb->AllRenderLines);

	//tnsZeroGeomtryBuffers(rb->Scene);

	while (reln = lstPopItem(&rb->VertexBufferPointers)) {
		FreeMem(reln->Pointer);
	}

	while (reln = lstPopItem(&rb->TriangleBufferPointers)) {
		FreeMem(reln->Pointer);
	}

	memStaticDestroy(&rb->RenderDataPool);
}

LANPR_RenderBuffer* lanpr_CreateRenderBuffer(SceneLANPR* lanpr) {
	if (lanpr->render_buffer) {
		lanpr_DestroyRenderData(lanpr->render_buffer);
		MEM_freeN(lanpr->render_buffer);
	}

	LANPR_RenderBuffer* rb = MEM_callocN(sizeof(LANPR_RenderBuffer), "creating LANPR render buffer");

	lanpr->render_buffer = rb;

	BLI_spin_init(&rb->csData);
	BLI_spin_init(&rb->csInfo);
	BLI_spin_init(&rb->csManagement);
	BLI_spin_init(&rb->RenderDataPool.csMem);

	return rb;
}



void lanpr_generate_geom_buffer(struct Object *ob){
    
}

static int lanpr_compute_feature_lines_exec(struct bContext *C, struct wmOperator *op){
	Scene *scene = CTX_data_scene(C);
	SceneLANPR* lanpr = &scene->lanpr;
    LANPR_RenderBuffer* rb;
	
	/* need threading, later.... */

    rb = lanpr_CreateRenderBuffer(lanpr);

	lanpr_MakeRenderGeometryBuffers(scene,scene->camera,rb);
	
	lanpr_CullTriangles(rb);

	lanpr_PerspectiveDivision(rb);

	lanpr_MakeInitialBoundingAreas(rb);

	lanpr_ComputeSceneContours(rb);

	lanpr_AddTriangles(rb);

	return OPERATOR_FINISHED;
}

static void lanpr_compute_feature_lines_cancel(struct bContext *C, struct wmOperator *op){

    return;
}


void SCENE_OT_lanpr_calculate_feature_lines(struct wmOperatorType* ot){

	/* identifiers */
	ot->name = "Calculate Feature Lines";
	ot->description = "LANPR calculates feature line in current scene";
	ot->idname = "SCENE_OT_lanpr_calculate";

	/* api callbacks */
	//ot->invoke = screen_render_invoke; /* why we need both invoke and exec? */
	//ot->modal = screen_render_modal;
	ot->cancel = lanpr_compute_feature_lines_cancel;
	ot->exec = lanpr_compute_feature_lines_exec;
}




/* internal */

LANPR_LineStyle* lanpr_new_line_layer(SceneLANPR* lanpr){
    LANPR_LineStyle* ls = MEM_callocN(sizeof(LANPR_LineStyle),"Line Style");
	BLI_addtail(&lanpr->line_style_layers,ls);
	lanpr->active_layer = ls;
	return ls;
}

static int lanpr_add_line_layer_exec(struct bContext *C, struct wmOperator *op){
	Scene *scene = CTX_data_scene(C);
	SceneLANPR* lanpr = &scene->lanpr;

    lanpr_new_line_layer(lanpr);

	return OPERATOR_FINISHED;
}


void SCENE_OT_lanpr_add_line_layer(struct wmOperatorType* ot){
	
	ot->name = "Add line layer";
	ot->description = "Add a new line layer";
	ot->idname = "SCENE_OT_lanpr_add_line_layer";

	ot->exec = lanpr_add_line_layer_exec;

}

static int lanpr_delete_line_layer_exec(struct bContext *C, struct wmOperator *op){

	return OPERATOR_FINISHED;
}


void SCENE_OT_lanpr_delete_line_layer(struct wmOperatorType* ot){
	
	ot->name = "Delete line layer";
	ot->description = "Delete selected line layer";
	ot->idname = "SCENE_OT_lanpr_delete_line_layer";

	ot->exec = lanpr_delete_line_layer_exec;
	
}