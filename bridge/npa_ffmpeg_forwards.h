/*
 * The pass-through entry points the 4.4.8 dylibs share.
 *
 * Both LibFFmpegCoreBridge.dylib and LibFFmpegFullBridge.dylib carry FFmpeg
 * 4.4.8 and forward the app's calls to it. The core dylib exports the
 * avformat/avcodec/avutil set; the full dylib exports that set plus the
 * scaler and resampler. Every one of them is a plain tail branch: 4.4.5 and
 * 4.4.8 share one ABI -- AVPixelFormat, AVSampleFormat and the public
 * structs are identical (see notes/ida-investigation.md section 8) -- so no
 * shim has to translate anything. The 9.0.2 dylib is the one that needs real
 * shims and does not include this file.
 *
 * The list lives here, once, expanded by whatever macro the including
 * translation unit passes in, so a symbol can never be added to one dylib
 * and forgotten in the other.
 */

#ifndef NPA_FFMPEG_FORWARDS_H
#define NPA_FFMPEG_FORWARDS_H

#define NPA_CORE_FORWARDS(F) \
    F(avformat_alloc_context) \
    F(avformat_open_input) \
    F(avformat_find_stream_info) \
    F(av_read_frame) \
    F(av_seek_frame) \
    F(av_find_input_format) \
    F(avformat_alloc_output_context2) \
    F(avformat_init_output) \
    F(avformat_write_header) \
    F(av_write_frame) \
    F(av_write_trailer) \
    F(avformat_new_stream) \
    F(avformat_free_context) \
    F(av_guess_format) \
    F(av_probe_input_buffer) \
    F(av_codec_get_tag) \
    F(avio_alloc_context) \
    F(avio_open2) \
    F(avio_read) \
    F(avio_write) \
    F(avio_flush) \
    F(avio_seek) \
    F(avio_size) \
    F(avio_close) \
    F(avio_closep) \
    F(avio_open_dyn_buf) \
    F(avio_close_dyn_buf) \
    F(avio_wl32) \
    F(avio_wb32) \
    F(av_find_default_stream_index) \
    F(av_index_search_timestamp) \
    F(avformat_close_input) \
    F(avformat_network_init) \
    F(avcodec_find_decoder) \
    F(avcodec_find_encoder) \
    F(avcodec_alloc_context3) \
    F(avcodec_free_context) \
    F(avcodec_open2) \
    F(avcodec_close) \
    F(avcodec_send_packet) \
    F(avcodec_receive_frame) \
    F(avcodec_flush_buffers) \
    F(avcodec_send_frame) \
    F(avcodec_receive_packet) \
    F(avcodec_fill_audio_frame) \
    F(avcodec_decode_subtitle2) \
    F(avcodec_get_name) \
    F(avcodec_descriptor_get) \
    F(avcodec_parameters_alloc) \
    F(avcodec_parameters_free) \
    F(avcodec_parameters_copy) \
    F(avcodec_parameters_from_context) \
    F(avcodec_parameters_to_context) \
    F(av_bsf_init) \
    F(av_bsf_flush) \
    F(av_bsf_free) \
    F(av_bsf_send_packet) \
    F(av_bsf_receive_packet) \
    F(av_bsf_alloc) \
    F(av_bsf_get_by_name) \
    F(av_init_packet) \
    F(av_packet_alloc) \
    F(av_packet_unref) \
    F(av_packet_free) \
    F(av_new_packet) \
    F(av_packet_copy_props) \
    F(av_packet_ref) \
    F(av_packet_move_ref) \
    F(av_packet_rescale_ts) \
    F(avsubtitle_free) \
    F(av_malloc) \
    F(av_mallocz) \
    F(av_realloc) \
    F(av_freep) \
    F(av_dict_get) \
    F(av_dict_set) \
    F(av_dict_free) \
    F(av_dict_copy) \
    F(av_frame_alloc) \
    F(av_frame_free) \
    F(av_frame_unref) \
    F(av_frame_ref) \
    F(av_image_alloc) \
    F(av_image_fill_arrays) \
    F(av_image_get_buffer_size) \
    F(av_image_copy) \
    F(av_reduce) \
    F(av_rescale_q) \
    F(av_log2) \
    F(av_get_bytes_per_sample) \
    F(av_sample_fmt_is_planar) \
    F(av_samples_get_buffer_size) \
    F(av_get_default_channel_layout) \
    F(av_get_channel_layout_channel_index) \
    F(av_opt_set) \
    F(av_gettime) \
    F(av_log_set_level) \
    F(ff_isom_write_hvcc) \
    F(avpriv_mpegaudio_decode_header)

#define NPA_UTIL_FORWARDS(F) \
    F(sws_alloc_context) \
    F(sws_getContext) \
    F(sws_getCachedContext) \
    F(sws_scale) \
    F(sws_freeContext) \
    F(swr_alloc) \
    F(swr_init) \
    F(swr_free) \
    F(swr_convert) \
    F(swr_set_matrix) \
    F(swr_alloc_set_opts)

#endif /* NPA_FFMPEG_FORWARDS_H */
