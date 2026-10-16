// Vehicles on Jolt (physics.h "Vehicles"): VehicleConstraint + WheeledVehicleController /
// MotorcycleController through joltc. Lives next to physics.c and shares its private state
// (physics_internal.h).
#include "physics_internal.h"
#include "core/log.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct PhysVehicle {
    BodyId body;
    JPH_VehicleConstraint *vc;
    JPH_VehicleController *ctl;
    JPH_VehicleCollisionTester *tester;
    JPH_WheelSettings *wheel_settings[PHYS_VEHICLE_MAX_WHEELS];
    JPH_LinearCurve *torque_curve, *long_friction, *lat_friction;
    JPH_VehicleTransmissionSettings *trans;
    JPH_VehicleControllerSettings *ctl_settings;
    int wheel_count;
    int kind;
    float roll_k, roll_c;   // motorcycle upright assist (N m / rad, N m s / rad), from the mass
    float grip_long[PHYS_VEHICLE_MAX_WHEELS], grip_lat[PHYS_VEHICLE_MAX_WHEELS];
    float slip_long[PHYS_VEHICLE_MAX_WHEELS], slip_lat[PHYS_VEHICLE_MAX_WHEELS];
};

// Per-wheel grip (tire pressure / health, surface, weather): scales the friction impulses.
static void JPH_API_CALL tire_impulse_cb(void *user, uint32_t wheel, float *out_long, float *out_lat, float susp_impulse, float long_friction,
                                          float lat_friction, float long_slip, float lat_slip, float dt) {
    (void)dt;
    PhysVehicle *v = (PhysVehicle *)user;
    float gl = 1.0f, gt = 1.0f;
    if (wheel < PHYS_VEHICLE_MAX_WHEELS) {
        gl = v->grip_long[wheel];
        gt = v->grip_lat[wheel];
        v->slip_long[wheel] = long_slip;
        v->slip_lat[wheel] = lat_slip;
    }
    *out_long = long_friction * susp_impulse * gl;
    *out_lat = lat_friction * susp_impulse * gt;
}

static JPH_Vec3 jvec(vec3 v) {
    JPH_Vec3 r = { v.x, v.y, v.z };
    return r;
}

PhysVehicle *phys_vehicle_create(PhysicsWorld *w, const PhysVehicleDesc *d) {
    PhysVehicle *v = (PhysVehicle *)calloc(1, sizeof(PhysVehicle));
    v->kind = d->kind;
    v->roll_k = (d->mass > 0 ? d->mass : 230.0f) * 9.81f * 1.6f;   // > m g h: holds it up when stopped
    v->roll_c = v->roll_k * 0.25f;
    v->wheel_count = DG_CLAMP(d->wheel_count, 2, PHYS_VEHICLE_MAX_WHEELS);
    for (int i = 0; i < PHYS_VEHICLE_MAX_WHEELS; i++) v->grip_long[i] = v->grip_lat[i] = 1.0f;
    // Body: a static compound of boxes (chassis, cabin / tank ...) with the centre of mass moved
    // down (stable; it tips over only when it really should).
    JPH_StaticCompoundShapeSettings *cs = JPH_StaticCompoundShapeSettings_Create();
    JPH_Quat qi = { 0, 0, 0, 1 };
    for (int i = 0; i < d->box_count && i < PHYS_VEHICLE_MAX_BOXES; i++) {
        JPH_Vec3 h = jvec(d->box_half[i]);
        float cr = fminf(0.05f, fminf(h.x, fminf(h.y, h.z)) * 0.5f);
        JPH_BoxShapeSettings *bs = JPH_BoxShapeSettings_Create(&h, cr);
        JPH_Vec3 p = jvec(d->box_center[i]);
        JPH_CompoundShapeSettings_AddShape((JPH_CompoundShapeSettings *)cs, &p, &qi, (const JPH_ShapeSettings *)bs, 0);
        JPH_ShapeSettings_Destroy((JPH_ShapeSettings *)bs);
    }
    JPH_Shape *compound = (JPH_Shape *)JPH_StaticCompoundShape_Create(cs);
    JPH_ShapeSettings_Destroy((JPH_ShapeSettings *)cs);
    JPH_Vec3 com = jvec(d->com_offset);
    JPH_Shape *shape = (JPH_Shape *)JPH_OffsetCenterOfMassShape_Create(&com, compound);
    JPH_Shape_Destroy(compound);
    JPH_RVec3 pos = { d->pos.x, d->pos.y, d->pos.z };
    JPH_Quat rot = { d->rot.x, d->rot.y, d->rot.z, d->rot.w };
    if (rot.x == 0 && rot.y == 0 && rot.z == 0 && rot.w == 0) rot.w = 1.0f;
    JPH_BodyCreationSettings *bcs = JPH_BodyCreationSettings_Create3(shape, &pos, &rot, JPH_MotionType_Dynamic, (JPH_ObjectLayer)PL_MOVING);
    JPH_BodyCreationSettings_SetUserData(bcs, d->user);
    JPH_BodyCreationSettings_SetFriction(bcs, 0.5f);
    JPH_BodyCreationSettings_SetLinearDamping(bcs, 0.02f);
    JPH_BodyCreationSettings_SetAngularDamping(bcs, 0.1f);
    JPH_BodyCreationSettings_SetMotionQuality(bcs, JPH_MotionQuality_LinearCast);
    JPH_MassProperties mp;
    memset(&mp, 0, sizeof(mp));
    JPH_Shape_GetMassProperties(shape, &mp);
    JPH_MassProperties_ScaleToMass(&mp, d->mass > 0 ? d->mass : 1200.0f);
    JPH_BodyCreationSettings_SetMassPropertiesOverride(bcs, &mp);
    JPH_BodyCreationSettings_SetOverrideMassProperties(bcs, JPH_OverrideMassProperties_MassAndInertiaProvided);
    v->body = JPH_BodyInterface_CreateAndAddBody(w->bi, bcs, JPH_Activation_Activate);
    JPH_BodyCreationSettings_Destroy(bcs);
    JPH_Shape_Destroy(shape);
    if (v->body == PHYS_INVALID_BODY) {
        log_error("physics: vehicle body creation failed");
        free(v);
        return NULL;
    }
    phys_record_body(w, v->body, d->box_count ? d->box_half[0] : v3(1, 0.5f, 2), PL_MOVING, PM_DYNAMIC, d->user, d->material);

    // Friction curves (slip ratio / slip angle -> friction coefficient).
    v->long_friction = JPH_LinearCurve_Create();
    JPH_LinearCurve_AddPoint(v->long_friction, 0.0f, 0.0f);
    JPH_LinearCurve_AddPoint(v->long_friction, 0.06f, 1.2f);
    JPH_LinearCurve_AddPoint(v->long_friction, 1.0f, 1.0f);
    v->lat_friction = JPH_LinearCurve_Create();
    JPH_LinearCurve_AddPoint(v->lat_friction, 0.0f, 0.0f);
    JPH_LinearCurve_AddPoint(v->lat_friction, 3.0f, 1.2f);
    JPH_LinearCurve_AddPoint(v->lat_friction, 20.0f, 1.0f);
    JPH_LinearCurve_AddPoint(v->lat_friction, 90.0f, 0.85f);
    JPH_WheelSettings *ws[PHYS_VEHICLE_MAX_WHEELS];
    for (int i = 0; i < v->wheel_count; i++) {
        const PhysWheelDesc *wd = &d->wheels[i];
        JPH_WheelSettingsWV *s = JPH_WheelSettingsWV_Create();
        JPH_WheelSettings *b = (JPH_WheelSettings *)s;
        JPH_Vec3 p = jvec(wd->pos);
        JPH_WheelSettings_SetPosition(b, &p);
        JPH_Vec3 fwd = { 0, 0, -1 }, up = { 0, 1, 0 }, down = { 0, -1, 0 };
        JPH_WheelSettings_SetWheelForward(b, &fwd);
        JPH_WheelSettings_SetWheelUp(b, &up);
        JPH_WheelSettings_SetSteeringAxis(b, &up);
        JPH_WheelSettings_SetSuspensionDirection(b, &down);
        JPH_WheelSettings_SetRadius(b, wd->radius);
        JPH_WheelSettings_SetWidth(b, wd->width);
        JPH_WheelSettings_SetSuspensionMinLength(b, wd->susp_min);
        JPH_WheelSettings_SetSuspensionMaxLength(b, wd->susp_max);
        JPH_SpringSettings sp = { JPH_SpringMode_FrequencyAndDamping, wd->susp_freq > 0 ? wd->susp_freq : 1.5f, wd->susp_damp > 0 ? wd->susp_damp : 0.5f };
        JPH_WheelSettings_SetSuspensionSpring(b, &sp);
        JPH_WheelSettingsWV_SetMaxSteerAngle(s, wd->max_steer_deg * DG_DEG2RAD);
        JPH_WheelSettingsWV_SetMaxBrakeTorque(s, wd->max_brake > 0 ? wd->max_brake : 1500.0f);
        JPH_WheelSettingsWV_SetMaxHandBrakeTorque(s, wd->max_handbrake);
        JPH_WheelSettingsWV_SetLongitudinalFriction(s, v->long_friction);
        JPH_WheelSettingsWV_SetLateralFriction(s, v->lat_friction);
        if (wd->inertia > 0) JPH_WheelSettingsWV_SetInertia(s, wd->inertia);
        ws[i] = b;
        v->wheel_settings[i] = b;
    }
    // Engine / transmission / differentials.
    JPH_WheeledVehicleControllerSettings *wc = d->kind == 1 ? (JPH_WheeledVehicleControllerSettings *)JPH_MotorcycleControllerSettings_Create()
                                                              : JPH_WheeledVehicleControllerSettings_Create();
    JPH_VehicleEngineSettings es;
    JPH_VehicleEngineSettings_Init(&es);
    es.maxTorque = d->max_torque > 0 ? d->max_torque : 500.0f;
    es.minRPM = d->min_rpm > 0 ? d->min_rpm : 1000.0f;
    es.maxRPM = d->max_rpm > 0 ? d->max_rpm : 6000.0f;
    v->torque_curve = JPH_LinearCurve_Create();
    JPH_LinearCurve_AddPoint(v->torque_curve, 0.0f, 0.8f);
    JPH_LinearCurve_AddPoint(v->torque_curve, 0.66f, 1.0f);
    JPH_LinearCurve_AddPoint(v->torque_curve, 1.0f, 0.8f);
    es.normalizedTorque = v->torque_curve;
    JPH_WheeledVehicleControllerSettings_SetEngine(wc, &es);
    v->trans = JPH_VehicleTransmissionSettings_Create();
    JPH_VehicleTransmissionSettings_SetMode(v->trans, JPH_TransmissionMode_Auto);
    if (d->gear_count > 0) JPH_VehicleTransmissionSettings_SetGearRatios(v->trans, d->gear_ratios, (uint32_t)d->gear_count);
    float rev = d->reverse_ratio != 0 ? d->reverse_ratio : -3.0f;
    JPH_VehicleTransmissionSettings_SetReverseGearRatios(v->trans, &rev, 1);
    JPH_VehicleTransmissionSettings_SetShiftUpRPM(v->trans, es.maxRPM * 0.7f);
    JPH_VehicleTransmissionSettings_SetShiftDownRPM(v->trans, es.minRPM * 1.8f);
    JPH_WheeledVehicleControllerSettings_SetTransmission(wc, v->trans);
    // Differentials: the driven axles (a motorcycle drives its rear wheel alone: left = right).
    int nd = 0;
    JPH_VehicleDifferentialSettings diffs[2];
    for (int a = 0; a < 2; a++) {
        if (d->diff_left[a] < 0) continue;
        JPH_VehicleDifferentialSettings_Init(&diffs[nd]);
        diffs[nd].leftWheel = d->diff_left[a];
        diffs[nd].rightWheel = d->diff_right[a];
        diffs[nd].differentialRatio = d->diff_ratio > 0 ? d->diff_ratio : 3.4f;
        nd++;
    }
    for (int i = 0; i < nd; i++) diffs[i].engineTorqueRatio = 1.0f / (float)nd;
    if (nd) JPH_WheeledVehicleControllerSettings_SetDifferentials(wc, diffs, (uint32_t)nd);
    if (d->kind == 1) {
        JPH_MotorcycleControllerSettings *ms = (JPH_MotorcycleControllerSettings *)wc;
        JPH_MotorcycleControllerSettings_SetMaxLeanAngle(ms, (d->max_lean_deg > 0 ? d->max_lean_deg : 45.0f) * DG_DEG2RAD);
        // Lean spring scaled to this body's roll inertia (Jolt's defaults suit a heavier, wider
        // sample bike; with a narrow body the damping term goes unstable at 60 Hz).
        float k = d->lean_spring > 0 ? d->lean_spring : 900.0f;
        JPH_MotorcycleControllerSettings_SetLeanSpringConstant(ms, k);
        JPH_MotorcycleControllerSettings_SetLeanSpringDamping(ms, k * 0.14f);
    }
    v->ctl_settings = (JPH_VehicleControllerSettings *)wc;
    JPH_VehicleConstraintSettings vs;
    JPH_VehicleConstraintSettings_Init(&vs);
    vs.up = (JPH_Vec3){ 0, 1, 0 };
    vs.forward = (JPH_Vec3){ 0, 0, -1 };
    vs.maxPitchRollAngle = (d->max_pitch_roll_deg > 0 ? d->max_pitch_roll_deg : 180.0f) * DG_DEG2RAD;
    vs.wheelsCount = (uint32_t)v->wheel_count;
    vs.wheels = ws;
    // Anti-roll bars on four-wheelers (wheel order: front left, front right, rear left, rear right).
    JPH_VehicleAntiRollBar bars[2];
    int nb = 0;
    if (d->kind == 0 && v->wheel_count == 4)
        for (int a = 0; a < 2; a++) {
            JPH_VehicleAntiRollBar_Init(&bars[nb]);
            bars[nb].leftWheel = a * 2;
            bars[nb].rightWheel = a * 2 + 1;
            bars[nb].stiffness = 1000.0f;
            nb++;
        }
    vs.antiRollBarsCount = (uint32_t)nb;
    vs.antiRollBars = nb ? bars : NULL;
    vs.controller = v->ctl_settings;
    JPH_BodyLockWrite lock;
    JPH_BodyLockInterface_LockWrite(w->locks, v->body, &lock);
    v->vc = JPH_VehicleConstraint_Create(lock.body, &vs);
    JPH_BodyLockInterface_UnlockWrite(w->locks, &lock);
    JPH_Vec3 up = { 0, 1, 0 };
    // Sphere casts for every vehicle (narrow spheres for motorcycle tires).
    v->tester = (JPH_VehicleCollisionTester *)JPH_VehicleCollisionTesterCastSphere_Create((JPH_ObjectLayer)PL_MOVING, 0.5f * d->wheels[0].width, &up, 80.0f * DG_DEG2RAD);
    JPH_VehicleConstraint_SetVehicleCollisionTester(v->vc, v->tester);
    JPH_PhysicsSystem_AddConstraint(w->system, (JPH_Constraint *)v->vc);
    JPH_PhysicsSystem_AddStepListener(w->system, JPH_VehicleConstraint_AsPhysicsStepListener(v->vc));
    v->ctl = JPH_VehicleConstraint_GetController(v->vc);
    if (d->kind == 1) JPH_MotorcycleController_EnableLeanController((JPH_MotorcycleController *)v->ctl, false);   // on at speed (phys_vehicle_moto_lean)
    JPH_WheeledVehicleController_SetTireMaxImpulseCallback((JPH_WheeledVehicleController *)v->ctl, tire_impulse_cb, v);
    return v;
}

void phys_vehicle_destroy(PhysicsWorld *w, PhysVehicle *v) {
    if (!v) return;
    if (v->vc) {
        JPH_PhysicsSystem_RemoveStepListener(w->system, JPH_VehicleConstraint_AsPhysicsStepListener(v->vc));
        JPH_PhysicsSystem_RemoveConstraint(w->system, (JPH_Constraint *)v->vc);
        JPH_Constraint_Destroy((JPH_Constraint *)v->vc);
    }
    if (v->tester) JPH_VehicleCollisionTester_Destroy(v->tester);
    phys_remove_body(w, v->body);
    for (int i = 0; i < v->wheel_count; i++) JPH_WheelSettings_Destroy(v->wheel_settings[i]);
    if (v->ctl_settings) JPH_VehicleControllerSettings_Destroy(v->ctl_settings);
    if (v->trans) JPH_VehicleTransmissionSettings_Destroy(v->trans);
    if (v->torque_curve) JPH_LinearCurve_Destroy(v->torque_curve);
    if (v->long_friction) JPH_LinearCurve_Destroy(v->long_friction);
    if (v->lat_friction) JPH_LinearCurve_Destroy(v->lat_friction);
    free(v);
}

BodyId phys_vehicle_body(const PhysVehicle *v) { return v ? v->body : PHYS_INVALID_BODY; }

// Motorcycles: Jolt's lean controller balances a moving bike but destabilizes one standing
// still, so it runs above walking pace; below it the bike is held upright (the rider's feet /
// the kickstand): a roll spring on the angular velocity.
void phys_vehicle_moto_lean(PhysicsWorld *w, PhysVehicle *v, float speed, float dt) {
    if (!v || v->kind != 1) return;
    bool on = speed > 3.0f;
    JPH_MotorcycleController_EnableLeanController((JPH_MotorcycleController *)v->ctl, on);
    if (on) return;
    JPH_RVec3 p;
    JPH_Quat q;
    JPH_BodyInterface_GetPositionAndRotation(w->bi, v->body, &p, &q);
    quat r = { q.x, q.y, q.z, q.w };
    vec3 fwd = quat_rotate(r, v3(0, 0, -1)), up = quat_rotate(r, v3(0, 1, 0));
    // Roll about the forward axis (signed), the roll rate, and a spring back to upright.
    vec3 flat_right = v3_norm(v3_cross(fwd, v3(0, 1, 0)));
    float roll = asinf(clampf(v3_dot(up, flat_right), -1.0f, 1.0f));   // + = rotated about +forward
    JPH_Vec3 av;
    JPH_BodyInterface_GetAngularVelocity(w->bi, v->body, &av);
    vec3 ang = v3(av.x, av.y, av.z);
    float rate = v3_dot(ang, fwd);
    // A torque (an angular velocity set here is lost in the vehicle constraint's own solve).
    (void)dt;
    float k = v->roll_k, c = v->roll_c;
    vec3 tq = v3_scale(fwd, -roll * k - rate * c);
    JPH_Vec3 t = { tq.x, tq.y, tq.z };
    JPH_BodyInterface_AddTorque(w->bi, v->body, &t);
}
int phys_vehicle_wheel_count(const PhysVehicle *v) { return v ? v->wheel_count : 0; }

void phys_vehicle_input(PhysicsWorld *w, PhysVehicle *v, float forward, float right, float brake, float handbrake) {
    if (!v) return;
    JPH_WheeledVehicleController_SetDriverInput((JPH_WheeledVehicleController *)v->ctl, clampf(forward, -1, 1), clampf(right, -1, 1), clampf(brake, 0, 1),
                                                clampf(handbrake, 0, 1));
    if (fabsf(forward) > 0.01f || fabsf(right) > 0.01f || brake > 0.01f) JPH_BodyInterface_ActivateBody(w->bi, v->body);
}

void phys_vehicle_set_grip(PhysVehicle *v, int wheel, float longitudinal, float lateral) {
    if (!v || wheel < 0 || wheel >= PHYS_VEHICLE_MAX_WHEELS) return;
    v->grip_long[wheel] = fmaxf(longitudinal, 0.0f);
    v->grip_lat[wheel] = fmaxf(lateral, 0.0f);
}

void phys_vehicle_state(PhysVehicle *v, PhysVehicleState *s) {
    memset(s, 0, sizeof(*s));
    if (!v) return;
    JPH_WheeledVehicleController *wc = (JPH_WheeledVehicleController *)v->ctl;
    const JPH_VehicleEngine *e = JPH_WheeledVehicleController_GetEngine(wc);
    const JPH_VehicleTransmission *t = JPH_WheeledVehicleController_GetTransmission(wc);
    s->rpm = JPH_VehicleEngine_GetCurrentRPM(e);
    s->gear = JPH_VehicleTransmission_GetCurrentGear(t);
    s->clutch = JPH_VehicleTransmission_GetClutchFriction(t);
    s->wheel_count = v->wheel_count;
    for (int i = 0; i < v->wheel_count; i++) {
        const JPH_Wheel *wh = JPH_VehicleConstraint_GetWheel(v->vc, (uint32_t)i);
        PhysWheelState *o = &s->wheel[i];
        o->rotation = JPH_Wheel_GetRotationAngle(wh);
        o->angular_velocity = JPH_Wheel_GetAngularVelocity(wh);
        o->steer = JPH_Wheel_GetSteerAngle(wh);
        o->suspension = JPH_Wheel_GetSuspensionLength(wh);
        o->contact = JPH_Wheel_HasContact(wh);
        o->contact_body = PHYS_INVALID_BODY;
        if (o->contact) {
            JPH_RVec3 cp;
            JPH_Vec3 cn;
            JPH_Wheel_GetContactPosition(wh, &cp);
            JPH_Wheel_GetContactNormal(wh, &cn);
            o->contact_pos = v3((float)cp.x, (float)cp.y, (float)cp.z);
            o->contact_normal = v3(cn.x, cn.y, cn.z);
            o->contact_body = JPH_Wheel_GetContactBodyID(wh);
        }
        o->slip_long = v->slip_long[i];
        o->slip_lat = v->slip_lat[i];
    }
}

void phys_vehicle_set_wheel_state(PhysVehicle *v, int i, float rotation, float angular_velocity) {
    if (!v || i < 0 || i >= v->wheel_count) return;
    JPH_Wheel *wh = JPH_VehicleConstraint_GetWheel(v->vc, (uint32_t)i);
    JPH_Wheel_SetRotationAngle(wh, rotation);
    JPH_Wheel_SetAngularVelocity(wh, angular_velocity);
}

void phys_vehicle_set_engine(PhysVehicle *v, float rpm, int gear) {
    if (!v) return;
    JPH_WheeledVehicleController *wc = (JPH_WheeledVehicleController *)v->ctl;
    JPH_VehicleEngine_SetCurrentRPM((JPH_VehicleEngine *)JPH_WheeledVehicleController_GetEngine(wc), rpm);
    JPH_VehicleTransmission_Set((JPH_VehicleTransmission *)JPH_WheeledVehicleController_GetTransmission(wc), gear, 1.0f);
}

// World transform of a wheel for rendering (the wheel model's axle along local X).
mat4 phys_vehicle_wheel_transform(PhysVehicle *v, int i) {
    mat4 m = mat4_identity();
    if (!v || i < 0 || i >= v->wheel_count) return m;
    JPH_Vec3 right = { 1, 0, 0 }, up = { 0, 1, 0 };
    JPH_RMat4 r;
    JPH_VehicleConstraint_GetWheelWorldTransform(v->vc, (uint32_t)i, &right, &up, &r);
    for (int c = 0; c < 3; c++) {
        m.m[c * 4 + 0] = r.column[c].x;
        m.m[c * 4 + 1] = r.column[c].y;
        m.m[c * 4 + 2] = r.column[c].z;
        m.m[c * 4 + 3] = 0.0f;
    }
#ifdef JPH_DOUBLE_PRECISION
    m.m[12] = (float)r.column3.x;
    m.m[13] = (float)r.column3.y;
    m.m[14] = (float)r.column3.z;
#else
    m.m[12] = r.column[3].x;
    m.m[13] = r.column[3].y;
    m.m[14] = r.column[3].z;
#endif
    m.m[15] = 1.0f;
    return m;
}
