
export void mul_float4x4_float4x4(uniform const float a[16], uniform const float b[16], uniform float c[16]) {
    uniform float<4> b_row0 = { b[0], b[1], b[2], b[3] };
    uniform float<4> b_row1 = {b[4], b[5], b[6], b[7]};
    uniform float<4> b_row2 = {b[8], b[9], b[10], b[11]};
    uniform float<4> b_row3 = {b[12], b[13], b[14], b[15]};

    uniform float<4> c_p4[4] = {
        a[0] * b_row0 + a[1] * b_row1 + a[2] * b_row2 + a[3] * b_row3,
        a[4] * b_row0 + a[5] * b_row1 + a[6] * b_row2 + a[7] * b_row3,
        a[8] * b_row0 + a[9] * b_row1 + a[10] * b_row2 + a[11] * b_row3,
        a[12] * b_row0 + a[13] * b_row1 + a[14] * b_row2 + a[15] * b_row3
    };

    memcpy(c, c_p4, sizeof(c_p4));
}

export void mul_float4_float4x4(uniform const float a[4], uniform const float b[16], uniform float c[4]) {
    uniform float<4> b_row0 = {b[0], b[1], b[2], b[3]};
    uniform float<4> b_row1 = {b[4], b[5], b[6], b[7]};
    uniform float<4> b_row2 = {b[8], b[9], b[10], b[11]};
    uniform float<4> b_row3 = {b[12], b[13], b[14], b[15]};

    uniform float<4> c_p4 = a[0] * b_row0 + a[1] * b_row1 + a[2] * b_row2 + a[3] * b_row3;
    memcpy(c, &c_p4, sizeof(c_p4));
}

export void float4x4_affine_inverse(uniform const float a[16], uniform float res[16]) {
    if(programIndex < 3) {
        float r0 = a[programIndex];
        float r1 = a[4 + programIndex];
        float r2 = a[8 + programIndex];
        float r3 = a[12 + programIndex];

        uniform float s[4];
        s[0] = reduce_add(r0 * r0);
        s[1] = reduce_add(r1 * r1);
        s[2] = reduce_add(r2 * r2);
        s[programIndex] = sqrt(s[programIndex]);

        r0 /= s[0];
        r1 /= s[1];
        r2 /= s[2];

        uniform const int perms[] = {1, 2, 0, 0, 2, 0, 1, 0};
        const int perm0 = perms[programIndex];
        const int perm1 = perms[4 + programIndex];

        float r0_yzx = shuffle(r0, perm0);
        float r0_zxy = shuffle(r0, perm1);
        float r1_yzx = shuffle(r1, perm0);
        float r1_zxy = shuffle(r1, perm1);
        float r2_yzx = shuffle(r2, perm0);
        float r2_zxy = shuffle(r2, perm1);

        float aff_r0 = r1_yzx * r2_zxy - r1_zxy * r2_yzx;
        float aff_r1 = r2_yzx * r0_zxy - r2_zxy * r0_yzx;
        float aff_r2 = r0_yzx * r1_zxy - r0_zxy * r1_yzx;
        float aff_r3 = 0.f;

        uniform float inv_det = 1.f / reduce_add(r0 * aff_r0);
        s[programIndex] = inv_det / s[programIndex];

        aff_r0 *= s[0];
        aff_r1 *= s[1];
        aff_r2 *= s[2];
        // aff_r0 *= inv_det;
        // aff_r1 *= inv_det;
        // aff_r2 *= inv_det;

        uniform float inv_r3[4];
        inv_r3[0] = -reduce_add(aff_r0 * r3);
        inv_r3[1] = -reduce_add(aff_r1 * r3);
        inv_r3[2] = -reduce_add(aff_r2 * r3);

        aff_r0 = insert(aff_r0, 3, -reduce_add(aff_r0 * r3));
        aff_r1 = insert(aff_r1, 3, -reduce_add(aff_r1 * r3));
        aff_r2 = insert(aff_r2, 3, -reduce_add(aff_r2 * r3));
        aff_r3 = insert(aff_r3, 3, 1.f);

        unmasked {
            if(programIndex < 4) {
                res[4 * programIndex] = aff_r0;
                res[4 * programIndex + 1] = aff_r1;
                res[4 * programIndex + 2] = aff_r2;
                res[4 * programIndex + 3] = aff_r3;
            }
        }
    }
}