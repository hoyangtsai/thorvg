/*
LodePNG Utils

Copyright (c) 2005-2020 Lode Vandevenne

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
    claim that you wrote the original software. If you use this software
    in a product, an acknowledgment in the product documentation would be
    appreciated but is not required.

    2. Altered source versions must be plainly marked as such, and must not be
    misrepresented as being the original software.

    3. This notice may not be removed or altered from any source
    distribution.
*/

#include <cmath>

#include "tvgAllocator.h"
#include "tvgLodePng.h"

namespace lodepng {

// THORVG: Suppress warnings from the copied LodePNG utility code.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif

// Only temporarily here until this is integrated into lodepng.c(pp)
#define LODEPNG_MAX(a, b) (((a) > (b)) ? (a) : (b))
#define LODEPNG_MIN(a, b) (((a) < (b)) ? (a) : (b))

/* Parameters of a tone reproduction curve, either with a power law formula or with a lookup table. */
typedef struct {
    unsigned type; /* 0=linear, 1=lut, 2 = simple gamma, 3-6 = parametric (matches ICC parametric types 1-4) */
    float* lut; /* for type 1 */
    size_t lut_size;
    float gamma; /* for type 2 and more */
    float a, b, c, d, e, f; /* parameters for type 3-7 */
} LodePNGICCCurve;

void lodepng_icc_curve_init(LodePNGICCCurve* curve) {
    curve->lut = 0;
    curve->lut_size = 0;
}

void lodepng_icc_curve_cleanup(LodePNGICCCurve* curve) {
    tvg::free(curve->lut);
}

/* Values parsed from ICC profile, see parseICC for more information about this subset.*/
typedef struct {
    /* 0 = color model not supported by PNG (CMYK, Lab, ...), 1 = gray, 2 = RGB */
    int inputspace;
    // THORVG: Omit the unused ICC profile version fields from this stripped conversion path.

    /* The whitepoint of the profile connection space (PCS). Should always be D50, but parsed and used anyway.
    (to be clear, whitepoint and illuminant are synonyms in practice, but here field "illuminant" is ICC's
    "global" whitepoint that is always D50, and the field "white" below allows deriving the whitepoint of
    the particular RGB space represented here) */
    float illuminant[3];

    /* if true, has chromatic adaptation matrix that must be used. If false, you must compute a chromatic adaptation
    matrix yourself from "illuminant" and "white". */
    unsigned has_chad;
    float chad[9]; /* chromatic adaptation matrix, if given */

    /* The whitepoint of the RGB color space as stored in the ICC file. If has_chad, must be adapted with the
    chad matrix to become the one we need to go to absolute XYZ (in fact ICC implies it should then be
    exactly D50 in the file, redundantly, before this transformation with chad), else use as-is (then its
    values can actually be something else than D50, and are the ones we need). */
    unsigned has_whitepoint;
    float white[3];
    /* Chromaticities of the RGB space in XYZ color space, but given such that you must still
    whitepoint adapt them from D50 to the RGB space whitepoint to go to absolute XYZ (if has_chad,
    with chad, else with bradford adaptation matrix from illuminant to white). */
    unsigned has_chromaticity;
    float red[3];
    float green[3];
    float blue[3];

    unsigned has_trc; /* TRC = tone reproduction curve (aka "gamma correction") */

    /* TRC's for the three channels (only first one used if grayscale) */
    LodePNGICCCurve trc[3];
} LodePNGICC;

void lodepng_icc_init(LodePNGICC* icc) {
    lodepng_icc_curve_init(&icc->trc[0]);
    lodepng_icc_curve_init(&icc->trc[1]);
    lodepng_icc_curve_init(&icc->trc[2]);
}

void lodepng_icc_cleanup(LodePNGICC* icc) {
    lodepng_icc_curve_cleanup(&icc->trc[0]);
    lodepng_icc_curve_cleanup(&icc->trc[1]);
    lodepng_icc_curve_cleanup(&icc->trc[2]);
}

/* ICC tone response curve, nonlinear (encoded) to linear.
Input and output in range 0-1. If color was integer 0-255, multiply with (1.0f/255)
to get the correct floating point behavior.
Outside of range 0-1, will not clip but either return x itself, or in cases
where it makes sense, a value defined by the same function.
NOTE: ICC requires clipping, but we do that only later when converting float to integer.*/
static float iccForwardTRC(const LodePNGICCCurve* curve, float x) {
    if(curve->type == 0) {
        return x;
    }
    if(curve->type == 1) { /* Lookup table */
        float v0, v1, fraction;
        size_t index;
        if(!curve->lut) return 0; /* error */
        if(x < 0) return x;
        index = (size_t)(x * (curve->lut_size - 1));
        if(index >= curve->lut_size) return x;

        /* LERP */
        v0 = curve->lut[index];
        v1 = (index + 1 < curve->lut_size) ? curve->lut[index + 1] : 1.0f;
        fraction = (x * (curve->lut_size - 1)) - index;
        return v0 * (1 - fraction) + v1 * fraction;
    }
    if(curve->type == 2) {
        /* Gamma expansion */
        return (x > 0) ? powf(x, curve->gamma) : x;
    }
    /* TODO: all the ones below are untested */
    if(curve->type == 3) {
        if(x < 0) return x;
        return x >= (-curve->b / curve->a) ? (powf(curve->a * x + curve->b, curve->gamma) + curve->c) : 0;
    }
    if(curve->type == 4) {
        if(x < 0) return x;
        return x >= (-curve->b / curve->a) ? (powf(curve->a * x + curve->b, curve->gamma) + curve->c) : curve->c;
    }
    if(curve->type == 5) {
        return x >= curve->d ? (powf(curve->a * x + curve->b, curve->gamma)) : (curve->c * x);
    }
    if(curve->type == 6) {
        return x >= curve->d ? (powf(curve->a * x + curve->b, curve->gamma) + curve->c) : (curve->c * x + curve->f);
    }
    return 0;
}

static unsigned decodeICCUint16(const unsigned char* data, size_t size, size_t* pos) {
    *pos += 2;
    if (*pos > size) return 0;
    return (unsigned)((data[*pos - 2] << 8) | (data[*pos - 1]));
}

static unsigned decodeICCUint32(const unsigned char* data, size_t size, size_t* pos) {
    *pos += 4;
    if (*pos > size) return 0;
    return (unsigned)((data[*pos - 4] << 24) | (data[*pos - 3] << 16) | (data[*pos - 2] << 8) | (data[*pos - 1] << 0));
}

static int decodeICCInt32(const unsigned char* data, size_t size, size_t* pos) {
    *pos += 4;
    if (*pos > size) return 0;
    /*TODO: this is incorrect if sizeof(int) != 4*/
    return (data[*pos - 4] << 24) | (data[*pos - 3] << 16) | (data[*pos - 2] << 8) | (data[*pos - 1] << 0);
}

static float decodeICC15Fixed16(const unsigned char* data, size_t size, size_t* pos) {
    return decodeICCInt32(data, size, pos) / 65536.0;
}

static unsigned isICCword(const unsigned char* data, size_t size, size_t pos, const char* word) {
    if(pos + 4 > size) return 0;
    return data[pos + 0] == (unsigned char)word[0] &&
        data[pos + 1] == (unsigned char)word[1] &&
        data[pos + 2] == (unsigned char)word[2] &&
        data[pos + 3] == (unsigned char)word[3];
}

/* Parses a subset of the ICC profile, supporting the necessary mix of ICC v2
and ICC v4 required to correctly convert the RGB color space to XYZ.
Does not parse values not related to this specific PNG-related purpose, and
does not support non-RGB profiles or lookup-table based chroma (but it
supports lookup tables for TRC aka "gamma"). */
static unsigned parseICC(LodePNGICC* icc, const unsigned char* data, size_t size) {
    size_t i, j;
    size_t pos = 0;
    unsigned inputspace;
    size_t numtags;

    if(size < 132) return 1; /* Too small to be a valid icc profile. */

    icc->has_chromaticity = 0;
    icc->has_whitepoint = 0;
    icc->has_trc = 0;
    icc->has_chad = 0;

    icc->trc[0].type = icc->trc[1].type = icc->trc[2].type = 0;
    icc->white[0] = icc->white[1] = icc->white[2] = 0;
    icc->red[0] = icc->red[1] = icc->red[2] = 0;
    icc->green[0] = icc->green[1] = icc->green[2] = 0;
    icc->blue[0] = icc->blue[1] = icc->blue[2] = 0;

    pos = 16;
    inputspace = decodeICCUint32(data, size, &pos);
    if(pos >= size) return 1;
    if(inputspace == 0x47524159) {
        /* The string  "GRAY" as unsigned 32-bit int. */
        icc->inputspace = 1;
    } else if(inputspace == 0x52474220) {
        /* The string  "RGB " as unsigned 32-bit int. */
        icc->inputspace = 2;
    } else {
        /* unsupported by PNG (CMYK, YCbCr, Lab, HSV, ...) */
        icc->inputspace = 0;
    }

    /* Should always be 0.9642, 1.0, 0.8249 */
    pos = 68;
    icc->illuminant[0] = decodeICC15Fixed16(data, size, &pos);
    icc->illuminant[1] = decodeICC15Fixed16(data, size, &pos);
    icc->illuminant[2] = decodeICC15Fixed16(data, size, &pos);

    pos = 128;
    numtags = decodeICCUint32(data, size, &pos);
    if(pos >= size) return 1;
    /* scan for tags we want to handle */
    for(i = 0; i < numtags; i++) {
        size_t offset;
        unsigned tagsize;
        size_t namepos = pos;
        pos += 4;
        offset = decodeICCUint32(data, size, &pos);
        tagsize = decodeICCUint32(data, size, &pos);
        if(pos >= size || offset >= size) return 1;
        if(offset + tagsize > size) return 1;
        if(tagsize < 8) return 1;

        if(isICCword(data, size, namepos, "wtpt")) {
            offset += 8; /* skip tag and reserved */
            icc->white[0] = decodeICC15Fixed16(data, size, &offset);
            icc->white[1] = decodeICC15Fixed16(data, size, &offset);
            icc->white[2] = decodeICC15Fixed16(data, size, &offset);
            icc->has_whitepoint = 1;
        } else if(isICCword(data, size, namepos, "rXYZ")) {
            offset += 8; /* skip tag and reserved */
            icc->red[0] = decodeICC15Fixed16(data, size, &offset);
            icc->red[1] = decodeICC15Fixed16(data, size, &offset);
            icc->red[2] = decodeICC15Fixed16(data, size, &offset);
            icc->has_chromaticity = 1;
        } else if(isICCword(data, size, namepos, "gXYZ")) {
            offset += 8; /* skip tag and reserved */
            icc->green[0] = decodeICC15Fixed16(data, size, &offset);
            icc->green[1] = decodeICC15Fixed16(data, size, &offset);
            icc->green[2] = decodeICC15Fixed16(data, size, &offset);
            icc->has_chromaticity = 1;
        } else if(isICCword(data, size, namepos, "bXYZ")) {
            offset += 8; /* skip tag and reserved */
            icc->blue[0] = decodeICC15Fixed16(data, size, &offset);
            icc->blue[1] = decodeICC15Fixed16(data, size, &offset);
            icc->blue[2] = decodeICC15Fixed16(data, size, &offset);
            icc->has_chromaticity = 1;
        } else if(isICCword(data, size, namepos, "chad")) {
            offset += 8; /* skip datatype keyword "sf32" and reserved */
            for(j = 0; j < 9; j++) {
                icc->chad[j] = decodeICC15Fixed16(data, size, &offset);
            }
            icc->has_chad = 1;
        } else if(isICCword(data, size, namepos, "rTRC") ||
            isICCword(data, size, namepos, "gTRC") ||
            isICCword(data, size, namepos, "bTRC") ||
            isICCword(data, size, namepos, "kTRC")) {
            char c = (char)data[namepos];
            /* both 'k' and 'r' are stored in channel 0 */
            int channel = (c == 'b') ? 2 : (c == 'g' ? 1 : 0);
            /* "curv": linear, gamma power or LUT */
            if(isICCword(data, size, offset, "curv")) {
                size_t count;
                LodePNGICCCurve* trc = &icc->trc[channel];
                icc->has_trc = 1;
                offset += 8; /* skip tag "curv" and reserved */
                count = decodeICCUint32(data, size, &offset);
                if(count == 0) {
                    trc->type = 0; /* linear */
                } else if(count == 1) {
                    trc->type = 2; /* gamma */
                    trc->gamma = decodeICCUint16(data, size, &offset) / 256.0f;
                } else {
                    trc->type = 1; /* LUT */
                    if(offset + count * 2 > size || count > 16777216) return 1; /* also avoid crazy count */
                    trc->lut_size = count;
                    trc->lut = (float*)tvg::malloc(count * sizeof(float));
                    for(j = 0; j < count; j++) {
                        trc->lut[j] = decodeICCUint16(data, size, &offset) * (1.0f / 65535.0f);
                    }
                }
            }
            /* "para": parametric formula with gamma power, multipliers, biases and comparison point */
            /* TODO: test this on a realistic sample */
            if(isICCword(data, size, offset, "para")) {
                unsigned type;
                LodePNGICCCurve* trc = &icc->trc[channel];
                icc->has_trc = 1;
                offset += 8; /* skip tag "para" and reserved */
                type = decodeICCUint16(data, size, &offset);
                offset += 2;
                if(type > 4) return 1; /* unknown parametric curve type */
                trc->type = type + 2;
                trc->gamma = decodeICC15Fixed16(data, size, &offset);
                if(type >= 1) {
                    trc->a = decodeICC15Fixed16(data, size, &offset);
                    trc->b = decodeICC15Fixed16(data, size, &offset);
                }
                if(type >= 2) {
                    trc->c = decodeICC15Fixed16(data, size, &offset);
                }
                if(type >= 3) {
                    trc->d = decodeICC15Fixed16(data, size, &offset);
                }
                if(type == 4) {
                    trc->e = decodeICC15Fixed16(data, size, &offset);
                    trc->f = decodeICC15Fixed16(data, size, &offset);
                }
            }
            /* TODO: verify: does the "chrm" tag participate in computation so should be parsed? */
        }
        /* Return error if any parse went beyond the filesize. Note that the
        parsing itself was always safe since it bound-checks inside. */
        if(offset > size) return 1;
    }

    return 0;
}

/* Multiplies 3 vector values with 3x3 matrix */
static void mulMatrix(float* x2, float* y2, float* z2, const float* m, double x, double y, double z) {
    /* double used as inputs even though in general the images are float, so the sums happen in
    double precision, because float can give numerical problems for nearby values */
    *x2 = x * m[0] + y * m[1] + z * m[2];
    *y2 = x * m[3] + y * m[4] + z * m[5];
    *z2 = x * m[6] + y * m[7] + z * m[8];
}

static void mulMatrixMatrix(float* result, const float* a, const float* b) {
    int i;
    float temp[9]; /* temp is to allow result and a or b to be the same */
    mulMatrix(&temp[0], &temp[3], &temp[6], a, b[0], b[3], b[6]);
    mulMatrix(&temp[1], &temp[4], &temp[7], a, b[1], b[4], b[7]);
    mulMatrix(&temp[2], &temp[5], &temp[8], a, b[2], b[5], b[8]);
    for(i = 0; i < 9; i++) result[i] = temp[i];
}

/* Inverts 3x3 matrix in place */
static unsigned invMatrix(float* m) {
    int i;
    /* double used instead of float for intermediate computations to avoid
    intermediate numerical precision issues */
    double e0 = (double)m[4] * m[8] - (double)m[5] * m[7];
    double e3 = (double)m[5] * m[6] - (double)m[3] * m[8];
    double e6 = (double)m[3] * m[7] - (double)m[4] * m[6];
    /* inverse determinant */
    double d = 1.0 / (m[0] * e0 + m[1] * e3 + m[2] * e6);
    float result[9];
    if((d > 0 ? d : -d) > 1e15) return 1; /* error, likely not invertible */
    result[0] = e0 * d;
    result[1] = ((double)m[2] * m[7] - (double)m[1] * m[8]) * d;
    result[2] = ((double)m[1] * m[5] - (double)m[2] * m[4]) * d;
    result[3] = e3 * d;
    result[4] = ((double)m[0] * m[8] - (double)m[2] * m[6]) * d;
    result[5] = ((double)m[3] * m[2] - (double)m[0] * m[5]) * d;
    result[6] = e6 * d;
    result[7] = ((double)m[6] * m[1] - (double)m[0] * m[7]) * d;
    result[8] = ((double)m[0] * m[4] - (double)m[3] * m[1]) * d;
    for(i = 0; i < 9; i++) m[i] = result[i];
    return 0; /* ok */
}

/* Get the matrix to go from linear RGB to XYZ given the RGB whitepoint and chromaticities in XYZ colorspace */
static unsigned getChrmMatrixXYZ(float* m,
                                 float wX, float wY, float wZ,
                                 float rX, float rY, float rZ,
                                 float gX, float gY, float gZ,
                                 float bX, float bY, float bZ) {
    float t[9];
    float rs, gs, bs;
    t[0] = rX; t[1] = gX; t[2] = bX;
    t[3] = rY; t[4] = gY; t[5] = bY;
    t[6] = rZ; t[7] = gZ; t[8] = bZ;
    if(invMatrix(t)) return 1; /* error, not invertible */
    mulMatrix(&rs, &gs, &bs, t, wX, wY, wZ);
    m[0] = rs * rX; m[1] = gs * gX; m[2] = bs * bX;
    m[3] = rs * rY; m[4] = gs * gY; m[5] = bs * bY;
    m[6] = rs * rZ; m[7] = gs * gZ; m[8] = bs * bZ;
    return 0;
}

// THORVG: Returns a matrix adapting source whitepoint 0 to destination whitepoint 1.
// LodePNG offers XYZ scaling, Bradford, and Von Kries but effectively uses Bradford, so this path is fixed to it.
static void getAdaptationMatrix(float* m,
                                float wx0, float wy0, float wz0,
                                float wx1, float wy1, float wz1) {
    int i;
    static const float bradford[9] = {
        0.8951, 0.2664, -0.1614,
        -0.7502, 1.7135, 0.0367,
        0.0389, -0.0685, 1.0296
    };
    static const float bradfordinv[9] = {
        0.9869929, -0.1470543, 0.1599627,
        0.4323053, 0.5183603, 0.0492912,
        -0.0085287, 0.0400428, 0.9684867
    };
    float rho0, gam0, bet0, rho1, gam1, bet1, rho2, gam2, bet2;
    mulMatrix(&rho0, &gam0, &bet0, bradford, wx0, wy0, wz0);
    mulMatrix(&rho1, &gam1, &bet1, bradford, wx1, wy1, wz1);
    rho2 = rho1 / rho0;
    gam2 = gam1 / gam0;
    bet2 = bet1 / bet0;
    /* Multiply diagonal matrix with Bradford */
    for(i = 0; i < 3; i++) {
        m[i + 0] = rho2 * bradford[i + 0];
        m[i + 3] = gam2 * bradford[i + 3];
        m[i + 6] = bet2 * bradford[i + 6];
    }
    mulMatrixMatrix(m, bradfordinv, m);
}

/* validate whether the ICC profile is supported here for PNG */
static unsigned validateICC(const LodePNGICC* icc) {
    /* disable for unsupported things in the icc profile */
    if(icc->inputspace == 0) return 0;
    /* if we didn't recognize both chrm and trc, then maybe the ICC uses data
    types not supported here yet, so fall back to not using it. */
    if(icc->inputspace == 2) {
        /* RGB profile should have chromaticities */
        if(!icc->has_chromaticity) return 0;
    }
    /* An ICC profile without whitepoint is invalid for the kind of profiles used here. */
    if(!icc->has_whitepoint) return 0;
    if(!icc->has_trc) return 0;
    return 1; /* ok */
}

/* Returns chromaticity matrix for given ICC profile, adapted from ICC's
global illuminant as necessary.
Also returns the profile's whitepoint.
In case of a gray profile (icc->inputspace == 1), the identity matrix will be returned
so in that case you could skip the transform. */
static unsigned getICCChrm(float m[9], float whitepoint[3], const LodePNGICC* icc) {
    size_t i;
    if(icc->inputspace == 2) { /* RGB profile */
        float red[3], green[3], blue[3];
        float white[3]; /* the whitepoint of the RGB color space (absolute) */
        /* Adaptation matrix a.
        This is an adaptation needed for ICC's file format (due to it using
        an internal global illuminant unrelated to the actual images) */
        float a[9] = {1,0,0, 0,1,0, 0,0,1};
        /* If the profile has chromatic adaptation matrix "chad", use that one,
        else compute it from the illuminant and whitepoint. */
        if(icc->has_chad) {
            for(i = 0; i < 9; i++) a[i] = icc->chad[i];
            invMatrix(a);
        } else {
            getAdaptationMatrix(a, icc->illuminant[0], icc->illuminant[1], icc->illuminant[2],
                                icc->white[0], icc->white[1], icc->white[2]);
        }
        /* If the profile has a chad, then also the RGB's whitepoint must also be adapted from it (and the one
        given is normally D50). If it did not have a chad, then the whitepoint given is already the adapted one. */
        if(icc->has_chad) {
            mulMatrix(&white[0], &white[1], &white[2], a, icc->white[0], icc->white[1], icc->white[2]);
        } else {
            for(i = 0; i < 3; i++) white[i] = icc->white[i];
        }

        mulMatrix(&red[0], &red[1], &red[2], a, icc->red[0], icc->red[1], icc->red[2]);
        mulMatrix(&green[0], &green[1], &green[2], a, icc->green[0], icc->green[1], icc->green[2]);
        mulMatrix(&blue[0], &blue[1], &blue[2], a, icc->blue[0], icc->blue[1], icc->blue[2]);

        if(getChrmMatrixXYZ(m, white[0], white[1], white[2], red[0], red[1], red[2],
                            green[0], green[1], green[2], blue[0], blue[1], blue[2])) {
            return 1; /* error computing matrix */
        }
        /* output absolute whitepoint of the original RGB model */
        whitepoint[0] = white[0];
        whitepoint[1] = white[1];
        whitepoint[2] = white[2];
    } else {
        /* output the unity matrix, for doing no transform */
        m[0] = m[4] = m[8] = 1;
        m[1] = m[2] = m[3] = m[5] = m[6] = m[7] = 0;
        /* grayscale, don't do anything. That means we are implicitely using equal energy whitepoint "E", indicate
        this to the output. */
        whitepoint[0] = whitepoint[1] = whitepoint[2] = 1;
    }
    return 0; /* success */
}

// THORVG: RGBA8 iCCP specialization of LodePNG's convertToXYZ() setup path.
static unsigned convertToXYZ_setup(float source[9], float whitepoint[3],
                                   float gammatable[3][256], const LodePNGState* state) {
    LodePNGICC icc;
    lodepng_icc_init(&icc);
    auto error = parseICC(&icc, state->info_png.iccp_profile, state->info_png.iccp_profile_size);
    if(error || !validateICC(&icc)) {
        error = 1;
        goto cleanup; /* corrupted ICC profile */
    }

    // THORVG: Skip convertToXYZ()'s lodepng_convert(); lodepng_decode() already
    // normalized the source image to the required RGBA8 buffer.

    // THORVG: Expanded from convertToXYZ()'s "Handle transfer function" block.
    // Build all three tables for a uniform pixel path; grayscale reuses one TRC.
    for(size_t c = 0; c < 3; c++) {
        auto channel = icc.inputspace == 1 ? 0 : c;

        // THORVG: convertToXYZ_gamma_table, n = 256 (8bit depth), use_icc path
        for(size_t i = 0; i < 256; i++) {
            gammatable[c][i] = iccForwardTRC(&icc.trc[channel], i * (1.0f / 255.0f));
        }
    }

    // THORVG: see convertToXYZ_chrm -> getChrm only use getICChrm (see use_icc path)
    error = getICCChrm(source, whitepoint, &icc);

cleanup:
    lodepng_icc_cleanup(&icc);
    return error;
}

// THORVG: Fixed sRGB specialization of LodePNG's convertFromXYZ() setup path.
// target is convertFromXYZ_chrm()'s "XYZ to linear RGB matrix".
static unsigned convertFromXYZ_setup(float target[9], const float whitepoint[3]) {
    // THORVG: From getChrm(): "the standard linear sRGB to XYZ matrix".
    target[0] = 0.4124564f; target[1] = 0.3575761f; target[2] = 0.1804375f;
    target[3] = 0.2126729f; target[4] = 0.7151522f; target[5] = 0.0721750f;
    target[6] = 0.0193339f; target[7] = 0.1191920f; target[8] = 0.9503041f;
    // THORVG: From getChrm(): sRGB's xyY whitepoint (0.3127, 0.3290, 1) in XYZ.
    static const float srgb_whitepoint[3] = {0.9504559270516716f, 1, 1.0890577507598784f};

    // THORVG: Invert sRGB-to-XYZ into XYZ-to-linear-sRGB.
    if(invMatrix(target)) return 1;

    /* for relative rendering intent (any except absolute "3"), must whitepoint adapt to the original whitepoint.
    this also ensures grayscale stays grayscale (with absolute, grayscale could become e.g. blue or sepia) */
    float adaptation[9];
    /* "white" = absolute whitepoint of the new target RGB space, read from the target color profile.
    "whitepoint" is original absolute whitepoint (input as parameter of this function) of an
    RGB space the XYZ data once had before it was converted to XYZ, in other words the whitepoint that
    we want to adapt our current data to to make sure values that had equal R==G==B in the old space have
    the same property now (white stays white and gray stays gray).
    Note: "absolute" whitepoint above means, can be used as-is, not needing further adaptation itself like icc.white does.*/
    getAdaptationMatrix(adaptation,
                        whitepoint[0], whitepoint[1], whitepoint[2],
                        srgb_whitepoint[0], srgb_whitepoint[1], srgb_whitepoint[2]);

    /* multiply the from xyz matrix with the adaptation matrix: in total,
    the resulting matrix first adapts in XYZ space, then converts to RGB*/
    mulMatrixMatrix(target, target, adaptation);
    return 0;
}

// THORVG: RGBA8 single-pixel path combining convertToXYZ()'s transfer function
// with convertToXYZ_chrm()'s linear-RGB-to-XYZ transform.
static inline void convertToXYZ_pixel(float* x, float* y, float* z,
                                      const unsigned char* in, const float source[9],
                                      const float gammatable[3][256]) {

    // 0. Linear RGB -> XYZ
    mulMatrix(x, y, z, source, gammatable[0][in[0]], gammatable[1][in[1]], gammatable[2][in[2]]);
}

// THORVG: sRGB RGBA8 single-pixel specialization of LodePNG's convertFromXYZ();
// direct packing skips its full-image temporary buffer and final lodepng_convert().
static inline void convertFromXYZ_pixel(unsigned char* out, const unsigned char* in,
                                        float x, float y, float z, const float target[9]) {
    // 1. XYZ -> Linear RGB
    float rgb[3];
    mulMatrix(&rgb[0], &rgb[1], &rgb[2], target, x, y, z);  // THORVG: Always runs; fixed sRGB target cannot take the gray/absolute bypass.

    for(size_t c = 0; c < 3; c++) {
        // 2. Linear RGB -> sRGB
        // THORVG: This expands convertFromXYZ()'s "Handle transfer function" call to
        // convertFromXYZ_gamma(); its float input uses powf() instead of a lookup table.
        auto& v = rgb[c];
        v = (v < 0.0031308f) ? (v * 12.92f) : (1.055f * powf(v, 1 / 2.4f) - 0.055f);

        // 3. float sRGB -> 8-bit RGB
        out[c] = (unsigned char)(0.5f + 255.0f * LODEPNG_MIN(LODEPNG_MAX(0.0f, v), 1.0f));
    }
    out[3] = in[3];
}

// THORVG: This customizes LodePNG's generic color conversion for ThorVG's
// fixed RGBA8 input and sRGB output.
unsigned convertToSrgb(unsigned char* out, const unsigned char* in,
                       unsigned w, unsigned h,
                       const LodePNGState* state_in) {
    // THORVG: Skip the temporary sRGB output state. Upstream copies info_raw only
    // to preserve the input format; ThorVG always outputs RGBA8 sRGB, 8bit.

    // --- start of the expanded convertRGBModel() path ---
    // THORVG: Expand convertRGBModel() into setup and per-pixel stages to avoid
    // its full-image buffers; lodepng_decode() already provides RGBA8.

    // THORVG: Skip modelsEqual(); this function is called only for iCCP input,
    // which upstream always treats as different from the default sRGB target.

    float source[9], whitepoint[3], gammatable[3][256]; // 256 = 8bit depth
    auto error = convertToXYZ_setup(source, whitepoint, gammatable, state_in);
    if(error) return error;

    float target[9];
    error = convertFromXYZ_setup(target, whitepoint);
    if(error) return error;

    // THORVG: The pixel specializations preserve the original transform order
    // without convertRGBModel()'s full-image temporary buffers.
    for(size_t i = 0, n = (size_t)w * h; i < n; i++) {
        float x, y, z;
        convertToXYZ_pixel(&x, &y, &z, in, source, gammatable);
        convertFromXYZ_pixel(out, in, x, y, z, target);
        out += 4;
        in += 4;
    }
    // --- end of the expanded convertRGBModel() path ---
    return 0;
}

#undef LODEPNG_MAX
#undef LODEPNG_MIN

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

} // namespace lodepng
