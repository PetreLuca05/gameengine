// Drag: the standard G1 / G7 / GS (sphere) drag-coefficient tables against Mach number, and the
// point-mass retardation a = ρ·Cd(M)·v²·π / (8·BC) with BC in kg/m² (lb/in² × 703.07). Pure.
#ifndef DG_BAL_DRAG_H
#define DG_BAL_DRAG_H

#include "core/base.h"

#define BAL_BC_SI 703.0696f   // lb/in² -> kg/m²

typedef enum BalDragModel {
    BAL_DRAG_EXP,      // a = k·v² with k fitted per cartridge (cheap, no transonic rise)
    BAL_DRAG_TABLE,    // the cartridge's G-table
} BalDragModel;

float bal_drag_cd(int curve, float mach);                       // BalDragCurve, linear interp
// Deceleration magnitude (m/s²) for an air-relative speed.
float bal_drag_decel(int curve, float bc, float speed, float rho, float sound_speed);
#endif
