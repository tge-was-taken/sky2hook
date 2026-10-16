// GS alpha-test state as a uniform struct + the test as a discard predicate (As = fragment alpha).
// ATST: 0 NEVER, 2 LESS, 3 LEQUAL, 5 GEQUAL, 6 GREATER, 7 NOTEQUAL; 1 ALWAYS / 4 EQUAL pass through.
// alphaPremul is the shader-side alpha prescale some GS ALPHA blend combos need (1.0 = no-op).
struct GsFrag { int atestMode; float atestRef; float alphaPremul; };
uniform GsFrag uGs;
// Shader-side output-alpha prescale some GS ALPHA blend combos need (alphaPremul = 1.0 is a no-op).
float ps2AlphaPremul(float a) { return a * uGs.alphaPremul; }

// Does the GS alpha test fail for this fragment alpha? `a` must already be premul'd and ps2AScale'd.
// Separate from the discard helper because the 3D path tests the fragment's own alpha while storing
// something else: an opaque draw is still alpha-tested, but writes full alpha to the framebuffer.
bool ps2AtestFails(float a)
{
    // atestRef arrives unscaled (AREF/255); scale it to match the ps2AScale'd fragment alpha.
    // ps2AScale comes from gs_unit.glsl, which every shader includes before this one.
    float ref = ps2AScale(uGs.atestRef);
    return uGs.atestMode == 0                 // NEVER
        || (uGs.atestMode == 2 && a >= ref)   // LESS
        || (uGs.atestMode == 3 && a >  ref)   // LEQUAL
        || (uGs.atestMode == 5 && a <  ref)   // GEQUAL
        || (uGs.atestMode == 6 && a <= ref)   // GREATER
        || (uGs.atestMode == 7 && a == ref);  // NOTEQUAL
}

// Prescale the output alpha, run the GS alpha test on it, and return it. Discards on test failure.
// Folds ps2AlphaPremul in so a draw's "premul then test" is one call: outA = ps2AtestDiscards(outA).
float ps2AtestDiscards(float a)
{
    a = ps2AlphaPremul(a);
    if (ps2AtestFails(a))
        discard;
    return a;
}
