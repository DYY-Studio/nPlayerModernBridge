#ifndef NPA_HIGHBIT_COPY_H
#define NPA_HIGHBIT_COPY_H

#include <stdint.h>

int npa_copy_p010_planes(
    const uint8_t *source_y,
    const uint8_t *source_uv,
    int source_y_stride,
    int source_uv_stride,
    uint8_t *destination_y,
    uint8_t *destination_uv,
    int destination_y_stride,
    int destination_uv_stride,
    int width,
    int height
);

int npa_pack_yuv420p10_to_p010(
    const uint8_t *source_y,
    const uint8_t *source_u,
    const uint8_t *source_v,
    int source_y_stride,
    int source_u_stride,
    int source_v_stride,
    uint8_t *destination_y,
    uint8_t *destination_uv,
    int destination_y_stride,
    int destination_uv_stride,
    int width,
    int height,
    int full_range
);

int npa_pack_yuvp10_to_biplanar(
    const uint8_t *source_y,
    const uint8_t *source_u,
    const uint8_t *source_v,
    int source_y_stride,
    int source_u_stride,
    int source_v_stride,
    uint8_t *destination_y,
    uint8_t *destination_uv,
    int destination_y_stride,
    int destination_uv_stride,
    int width,
    int height,
    int horizontal_subsample,
    int vertical_subsample,
    int full_range
);

int npa_pack_yuvp16_to_biplanar16(
    const uint8_t *source_y,
    const uint8_t *source_u,
    const uint8_t *source_v,
    int source_y_stride,
    int source_u_stride,
    int source_v_stride,
    uint8_t *destination_y,
    uint8_t *destination_uv,
    int destination_y_stride,
    int destination_uv_stride,
    int width,
    int height,
    int horizontal_subsample,
    int vertical_subsample,
    int input_bit_depth,
    int full_range
);

#endif
