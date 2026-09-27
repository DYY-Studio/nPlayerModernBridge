/*
 * The entry points the ffmpeg-out448 unit forwards into its 4.4.8 closure.
 *
 * This dylib carries the output side of the app's FFmpeg use -- the HLS
 * session's transmux/transcode path (with that session's own demux and
 * decode), the SPDIF / IEC 61937 mux and the poster path's MJPEG encoder --
 * and every entry point is a plain tail branch into the closure, because
 * 4.4.5 and 4.4.8 share one ABI.
 *
 * The three faces keep separate entry-point prefixes (`npa_hls_`, `npa_spdif_`,
 * `npa_mjpeg_`) because a bridge dylib declares one entry point per API: the
 * payload resolves a call site through its own unit's name, and the export-set
 * check compares the built dylib against the manifest's declared names, so a
 * name shared by two domains would make that list ambiguous. The lines below
 * carry the bare FFmpeg symbol; the prefix is added by the including macro.
 */

#ifndef NPA_FFMPEG_OUT448_FORWARDS_H
#define NPA_FFMPEG_OUT448_FORWARDS_H

#define NPA_HLS_FORWARDS(F) \
    F(av_bsf_alloc) \
    F(av_bsf_free) \
    F(av_bsf_get_by_name) \
    F(av_bsf_init) \
    F(av_bsf_receive_packet) \
    F(av_bsf_send_packet) \
    F(av_codec_get_tag) \
    F(av_dict_copy) \
    F(av_dict_free) \
    F(av_dict_get) \
    F(av_dict_set) \
    F(av_frame_alloc) \
    F(av_frame_free) \
    F(av_freep) \
    F(av_get_default_channel_layout) \
    F(av_guess_format) \
    F(av_init_packet) \
    F(av_malloc) \
    F(av_new_packet) \
    F(av_opt_set) \
    F(av_packet_copy_props) \
    F(av_packet_move_ref) \
    F(av_packet_ref) \
    F(av_packet_rescale_ts) \
    F(av_packet_unref) \
    F(av_read_frame) \
    F(av_rescale_q) \
    F(av_samples_get_buffer_size) \
    F(av_seek_frame) \
    F(av_write_frame) \
    F(av_write_trailer) \
    F(avcodec_alloc_context3) \
    F(avcodec_close) \
    F(avcodec_fill_audio_frame) \
    F(avcodec_find_decoder) \
    F(avcodec_find_encoder) \
    F(avcodec_flush_buffers) \
    F(avcodec_free_context) \
    F(avcodec_open2) \
    F(avcodec_parameters_copy) \
    F(avcodec_parameters_from_context) \
    F(avcodec_parameters_to_context) \
    F(avcodec_receive_frame) \
    F(avcodec_receive_packet) \
    F(avcodec_send_frame) \
    F(avcodec_send_packet) \
    F(avformat_alloc_context) \
    F(avformat_alloc_output_context2) \
    F(avformat_close_input) \
    F(avformat_find_stream_info) \
    F(avformat_free_context) \
    F(avformat_init_output) \
    F(avformat_new_stream) \
    F(avformat_open_input) \
    F(avformat_write_header) \
    F(avio_alloc_context) \
    F(avio_close) \
    F(avio_close_dyn_buf) \
    F(avio_closep) \
    F(avio_flush) \
    F(avio_open2) \
    F(avio_open_dyn_buf) \
    F(avio_size) \
    F(avio_wb32) \
    F(avio_wl32) \
    F(avio_write)
#define NPA_SPDIF_FORWARDS(F) \
    F(av_freep) \
    F(av_init_packet) \
    F(av_malloc) \
    F(av_write_frame) \
    F(av_write_trailer) \
    F(avformat_alloc_output_context2) \
    F(avformat_free_context) \
    F(avformat_new_stream) \
    F(avformat_write_header) \
    F(avio_alloc_context) \
    F(avio_flush)
#define NPA_MJPEG_FORWARDS(F) \
    F(av_frame_alloc) \
    F(av_frame_free) \
    F(av_freep) \
    F(av_image_alloc) \
    F(av_init_packet) \
    F(av_packet_ref) \
    F(av_packet_unref) \
    F(avcodec_alloc_context3) \
    F(avcodec_find_encoder) \
    F(avcodec_free_context) \
    F(avcodec_open2) \
    F(avcodec_receive_packet) \
    F(avcodec_send_frame)

#endif /* NPA_FFMPEG_OUT448_FORWARDS_H */
