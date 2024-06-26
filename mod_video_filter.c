/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2017, Seven Du <dujinfang@gmail.com>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Seven Du <dujinfang@gmail.com>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Seven Du <dujinfang@gmail.com>
 * Anthony Minessale <anthm@freeswitch.org>
 *
 * mod_video_filter -- FS Video Codec / File Format using libav.org
 *
 */

#include <switch.h>

switch_loadable_module_interface_t *MODULE_INTERFACE;

SWITCH_MODULE_LOAD_FUNCTION(mod_video_filter_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_video_filter_shutdown);
SWITCH_MODULE_DEFINITION(mod_video_filter, mod_video_filter_load, mod_video_filter_shutdown, NULL);

typedef struct chromakey_context_s {
	int threshold;
	switch_image_t *bgimg;
	switch_image_t *bgimg_orig;
	switch_image_t *bgimg_scaled;
	switch_image_t *fgimg_scaled;
	switch_image_t *imgfg;
	switch_image_t *imgbg;
	void *data;
	void *patch_data;
	switch_size_t datalen;
	switch_size_t patch_datalen;
	switch_file_handle_t vfh;
	switch_file_handle_t fg_vfh;
	switch_rgb_color_t bgcolor;
	switch_core_session_t *session;
	switch_mutex_t *command_mutex;
	int patch;
	int mod;
	switch_chromakey_t *ck;
	switch_core_video_filter_t video_filters;
	switch_queue_t *child_queue;
	char *child_uuid;
	switch_media_bug_t *child_bug;
} chromakey_context_t;

typedef struct chromakey_child_context_s {
	chromakey_context_t *parent;
	char *master_uuid;
} chromakey_child_context_t;


typedef struct video_replace_context_s {
	switch_image_t *rp_img;
	switch_file_handle_t vfh;
	switch_core_session_t *session;
	int sound;
} video_replace_context_t;


static void init_context(chromakey_context_t *context)
{
	switch_color_set_rgb(&context->bgcolor, "#000000");
	context->threshold = 300;
	switch_mutex_init(&context->command_mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(context->session));
	switch_chromakey_create(&context->ck);
}

static void uninit_context(chromakey_context_t *context)
{
	switch_img_free(&context->bgimg);
	switch_img_free(&context->bgimg_orig);
	switch_img_free(&context->bgimg_scaled);
	switch_img_free(&context->fgimg_scaled);
	switch_img_free(&context->imgbg);
	switch_img_free(&context->imgfg);

	if (context->child_bug) {
		switch_core_media_bug_close(&context->child_bug, SWITCH_TRUE);
		context->child_uuid = NULL;
	}
	
	if (switch_test_flag(&context->vfh, SWITCH_FILE_OPEN)) {
		switch_core_file_close(&context->vfh);
		memset(&context->vfh, 0, sizeof(context->vfh));
	}

	if (switch_test_flag(&context->fg_vfh, SWITCH_FILE_OPEN)) {
		switch_core_file_close(&context->fg_vfh);
		memset(&context->vfh, 0, sizeof(context->fg_vfh));
	}

	switch_safe_free(context->data);
	switch_safe_free(context->patch_data);
	switch_chromakey_destroy(&context->ck);
}

// static int flush_video_queue(switch_queue_t *q, int min)
// {
// 	void *pop;

// 	if (switch_queue_size(q) > min) {
// 		while (switch_queue_trypop(q, &pop) == SWITCH_STATUS_SUCCESS) {
// 			switch_image_t *img = (switch_image_t *) pop;
// 			switch_img_free(&img);
// 			if (min && switch_queue_size(q) <= min) {
// 				break;
// 			}
// 		}
// 	}

// 	return switch_queue_size(q);
// }

static switch_bool_t chromakey_child_bug_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	chromakey_child_context_t *child_context = (chromakey_child_context_t *)user_data;

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
		{
		}
		break;
	case SWITCH_ABC_TYPE_CLOSE:
		{
		}
		break;
	case SWITCH_ABC_TYPE_READ_VIDEO_PING:
	case SWITCH_ABC_TYPE_VIDEO_PATCH:
		{
			switch_image_t *img = NULL;
			switch_frame_t *frame = switch_core_media_bug_get_video_ping_frame(bug);

			if (frame && frame->img) {
				switch_img_copy(frame->img, &img);
				if (switch_queue_trypush(child_context->parent->child_queue, img) != SWITCH_STATUS_SUCCESS) {
					switch_img_free(&img);
				}
				img = NULL;
			}
		}
		break;
	default:
		break;
	}

	return SWITCH_TRUE;
}




static void parse_params(chromakey_context_t *context, int start, int argc, char **argv, const char **function, switch_media_bug_flag_t *flags)
{
	int n = argc - start;
	int i = start;

	switch_mutex_lock(context->command_mutex);

	if (!context->ck) {
		switch_mutex_unlock(context->command_mutex);
		return;
	}
	
	context->patch = 0;

	if (n > 0 && argv[i]) { // color
		int j = 0;
		char *list[CHROMAKEY_MAX_MASK];
		int list_argc;

		list_argc = switch_split(argv[i], ':', list);

		switch_chromakey_clear_colors(context->ck);

		for (j = 0; j < list_argc; j++) {
			char *p;
			int thresh = 0;
			switch_rgb_color_t color = { 0 };
			
			if ((p = strchr(list[j], '+'))) {
				*p++ = '\0';
				thresh = atoi(p);
				if (thresh < 0) thresh = 0;
			}
	
			if (*list[j] == '#') {
				switch_color_set_rgb(&color, list[j]);
				switch_chromakey_add_color(context->ck, &color, thresh);
			} else {
				switch_chromakey_autocolor(context->ck, switch_chromakey_str2shade(context->ck, list[j]), thresh);
			}
		}
	}

	i++;

	if (n > 1 && argv[i]) { // thresh
		int thresh = atoi(argv[i]);

		if (thresh > 0) {
			switch_chromakey_set_default_threshold(context->ck, thresh);
		}
	}

	i++;

	if (n > 2 && argv[i]) {

		if (context->child_bug) {
			switch_core_media_bug_close(&context->child_bug, SWITCH_TRUE);
			context->child_uuid = NULL;
		}
		
		if (switch_test_flag(&context->vfh, SWITCH_FILE_OPEN)) {
			switch_core_file_close(&context->vfh);
			memset(&context->vfh, 0, sizeof(context->vfh));
		}

		if (context->bgimg_orig) {
			switch_img_free(&context->bgimg_orig);
		}

		if (context->bgimg) {
			switch_img_free(&context->bgimg);
		}

		if (context->bgimg_scaled) {
			switch_img_free(&context->bgimg_scaled);
		}

		if (context->fgimg_scaled) {
			switch_img_free(&context->fgimg_scaled);
		}

		
		if (argv[i][0] == '#') { // bgcolor
			switch_color_set_rgb(&context->bgcolor, argv[i]);
		} else if (!strncasecmp(argv[i], "uuid:", 5)) {
			char *uuid = argv[i] + 5;
			switch_core_session_t *bsession;
			switch_status_t status;

			
			if (!zstr(uuid) && (bsession = switch_core_session_locate(uuid))) {
				switch_channel_t *channel = switch_core_session_get_channel(bsession);
				chromakey_child_context_t *child_context;
				switch_media_bug_flag_t flags = SMBF_READ_VIDEO_PING|SMBF_READ_VIDEO_PATCH;
	
				switch_channel_wait_for_flag(channel, CF_VIDEO_READY, SWITCH_TRUE, 10000, NULL);
				
				child_context = (chromakey_child_context_t *) switch_core_session_alloc(bsession, sizeof(*child_context));
				child_context->master_uuid = switch_core_session_strdup(bsession, switch_core_session_get_uuid(context->session));
				
				
				if ((status = switch_core_media_bug_add(bsession, "chromakey_child", NULL,
														chromakey_child_bug_callback, child_context, 0, flags,
														&context->child_bug)) == SWITCH_STATUS_SUCCESS) {

					switch_queue_create(&context->child_queue, 200, switch_core_session_get_pool(context->session));
					child_context->parent = context;
					context->child_uuid = switch_core_session_strdup(context->session, switch_core_session_get_uuid(bsession));
				} else {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(bsession), SWITCH_LOG_ERROR, "Failure! %d\n", status);
				}

				switch_core_session_rwunlock(bsession);
			}
			
		} else if (switch_stristr(".png", argv[i])) {
			if (!(context->bgimg_orig = switch_img_read_png(argv[i], SWITCH_IMG_FMT_ARGB))) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error opening png\n");
			}
		} else {

			if (switch_core_file_open(&context->vfh, argv[i], 1, 8000,
									  SWITCH_FILE_FLAG_READ | SWITCH_FILE_DATA_SHORT | SWITCH_FILE_FLAG_VIDEO, 
									  switch_core_session_get_pool(context->session)) != SWITCH_STATUS_SUCCESS) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error opening video file\n");
			} else {
				switch_vid_params_t vp = { 0 };

				switch_core_media_get_vid_params(context->session, &vp);
				context->vfh.mm.scale_w = vp.width;
				context->vfh.mm.scale_h = vp.height;
				context->vfh.mm.fps = vp.fps;
			}
		}
	}



	while (n > 3 && argv[i]) {

		if (!strncasecmp(argv[i], "filter:", 7)) {
			char *filter = argv[i] + 7;
			switch_core_video_parse_filter_string(&context->video_filters, filter);

			if (context->bgimg_orig && context->video_filters) {
				switch_img_free(&context->bgimg);
				switch_img_copy(context->bgimg_orig, &context->bgimg);
				
				if (context->video_filters & SCV_FILTER_SEPIA_BG) {
					switch_img_sepia(context->bgimg, 0, 0, context->bgimg->d_w, context->bgimg->d_h);
				}
			
				if (context->video_filters & SCV_FILTER_GRAY_BG) {
					switch_img_sepia(context->bgimg, 0, 0, context->bgimg->d_w, context->bgimg->d_h);
				}
			}
		}


		if (!strncasecmp(argv[i], "fgvid:", 6)) {
			char *file = argv[i] + 6;

			if (switch_test_flag(&context->fg_vfh, SWITCH_FILE_OPEN)) {
				switch_core_file_close(&context->fg_vfh);
				memset(&context->fg_vfh, 0, sizeof(context->fg_vfh));
			}

			if (!zstr(file)) {
				if (switch_core_file_open(&context->fg_vfh, file, 1, 8000,
										  SWITCH_FILE_FLAG_READ | SWITCH_FILE_DATA_SHORT | SWITCH_FILE_FLAG_VIDEO, 
										  switch_core_session_get_pool(context->session)) != SWITCH_STATUS_SUCCESS) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error opening video file\n");
				} else {
					switch_vid_params_t vp = { 0 };

					switch_core_media_get_vid_params(context->session, &vp);
					context->fg_vfh.mm.scale_w = vp.width;
					context->fg_vfh.mm.scale_h = vp.height;
					context->fg_vfh.mm.fps = vp.fps;
				}
			}
		}

		if (!strncasecmp(argv[i], "fg:", 3)) {
			switch_img_free(&context->imgfg);
			if (!zstr(argv[i]+3)) {
				context->imgfg = switch_img_read_png(argv[i]+3, SWITCH_IMG_FMT_ARGB);
			}
		}

		if (!strncasecmp(argv[i], "bg:", 3)) {
			switch_img_free(&context->imgbg);
			if (!zstr(argv[i]+3)) {
				context->imgbg = switch_img_read_png(argv[i]+3, SWITCH_IMG_FMT_ARGB);
			}
		}

		if (!strcasecmp(argv[i], "patch")) {
			*function = "patch:video";
			*flags = SMBF_VIDEO_PATCH;
			context->patch++;
		}

		i++;
	}

	if (context->bgimg_orig && !context->bgimg) {
		switch_img_copy(context->bgimg_orig, &context->bgimg);
	}

	switch_core_session_request_video_refresh(context->session);
	context->mod++;

	switch_mutex_unlock(context->command_mutex);



}


//TODO: MODIFY HERE
static switch_status_t video_thread_callback(switch_core_session_t *session, switch_frame_t *frame, void *user_data)
{
    chromakey_context_t *context = (chromakey_context_t *)user_data;
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_image_t *img = NULL;
    switch_size_t bytes;
    void *patch_data;

    if (!switch_channel_ready(channel)) {
        return SWITCH_STATUS_FALSE;
    }

    if (!frame->img) {
        return SWITCH_STATUS_SUCCESS;
    }

    if (!context->patch && context->mod && !switch_test_flag(frame, SFF_IS_KEYFRAME)) {
        switch_core_session_request_video_refresh(context->session);
        return SWITCH_STATUS_SUCCESS;
    }

    context->mod = 0;

    if (switch_mutex_trylock(context->command_mutex) != SWITCH_STATUS_SUCCESS) {
        switch_image_t *last_img = switch_chromakey_cache_image(context->ck);

        if (last_img) {
            switch_img_patch(frame->img, last_img, 0, 0);
        }

        return SWITCH_STATUS_SUCCESS;
    }

    bytes = frame->img->d_w * frame->img->d_h * 4;

    if (bytes > context->datalen) {
        context->data = realloc(context->data, bytes);
        context->datalen = bytes;
    }

    switch_assert(context->data);

    patch_data = context->data;

    switch_img_to_raw(frame->img, context->data, frame->img->d_w * 4, SWITCH_IMG_FMT_ARGB);
    img = switch_img_wrap(NULL, SWITCH_IMG_FMT_ARGB, frame->img->d_w, frame->img->d_h, 1, context->data);

    switch_assert(img);

    // Convert to sepia
    switch_img_sepia(img, 0, 0, img->d_w, img->d_h);

    // XOR operation on the image data
    // for (int i = 0; i < img->d_h; ++i) {
    //     for (int j = 0; j < img->d_w; ++j) {
    //         unsigned char *pixel = &img->planes[SWITCH_PLANE_PACKED][i * img->stride[SWITCH_PLANE_PACKED] + j * 4];
    //         // XOR each color component (ARGB format)
    //         pixel[0] ^= 255; // alpha
    //         pixel[1] ^= 255; // red
    //         pixel[2] ^= 255; // green
    //         pixel[3] ^= 255; // blue
    //     }
    // }

    // Convert back to ARGB format
    switch_img_from_raw(&frame->img, patch_data, SWITCH_IMG_FMT_ARGB, frame->img->d_w, frame->img->d_h);

    switch_img_free(&img);

    switch_mutex_unlock(context->command_mutex);

    return SWITCH_STATUS_SUCCESS;
}


static switch_bool_t chromakey_bug_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	chromakey_context_t *context = (chromakey_context_t *)user_data;

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
		{
		}
		break;
	case SWITCH_ABC_TYPE_CLOSE:
		{
			switch_thread_rwlock_unlock(MODULE_INTERFACE->rwlock);
			uninit_context(context);
		}
		break;
	case SWITCH_ABC_TYPE_READ_VIDEO_PING:
	case SWITCH_ABC_TYPE_VIDEO_PATCH:
		{
			switch_frame_t *frame = switch_core_media_bug_get_video_ping_frame(bug);
			video_thread_callback(context->session, frame, context);
		}
		break;
	default:
		break;
	}

	return SWITCH_TRUE;
}

#define CHROMAKEY_APP_SYNTAX "<#mask_color> [threshold] [#bg_color|path/to/image.png]"
SWITCH_STANDARD_APP(chromakey_start_function)
{
	switch_media_bug_t *bug;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	char *argv[4] = { 0 };
	int argc;
	char *lbuf;
	switch_media_bug_flag_t flags = SMBF_READ_VIDEO_PING|SMBF_READ_VIDEO_PATCH;
	const char *function = "chromakey";
	chromakey_context_t *context;

	if ((bug = (switch_media_bug_t *) switch_channel_get_private(channel, "_chromakey_bug_"))) {
		if (!zstr(data) && !strcasecmp(data, "stop")) {
			switch_channel_set_private(channel, "_chromakey_bug_", NULL);
			switch_core_media_bug_remove(session, &bug);
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Cannot run 2 chromakey at once on the same channel!\n");
		}
		return;
	}

	switch_channel_wait_for_flag(channel, CF_VIDEO_READY, SWITCH_TRUE, 10000, NULL);

	context = (chromakey_context_t *) switch_core_session_alloc(session, sizeof(*context));
	switch_assert(context != NULL);
	memset(context, 0, sizeof(*context));
	context->session = session;
	init_context(context);

    if (data && (lbuf = switch_core_session_strdup(session, data))
        && (argc = switch_separate_string(lbuf, ' ', argv, (sizeof(argv) / sizeof(argv[0]))))) {
        parse_params(context, 1, argc, argv, &function, &flags);
    }

	switch_thread_rwlock_rdlock(MODULE_INTERFACE->rwlock);

	if (switch_core_media_bug_add(session, function, NULL, chromakey_bug_callback, context, 0, flags, &bug) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failure!\n");
		switch_thread_rwlock_unlock(MODULE_INTERFACE->rwlock);
		return;
	}

	switch_channel_set_private(channel, "_chromakey_bug_", bug);
}

/* API Interface Function */
#define CHROMAKEY_API_SYNTAX "<uuid> [start|stop] " CHROMAKEY_APP_SYNTAX
SWITCH_STANDARD_API(chromakey_api_function)
{
	switch_core_session_t *rsession = NULL;
	switch_channel_t *channel = NULL;
	switch_media_bug_t *bug;
	chromakey_context_t *context;
	char *mycmd = NULL;
	int argc = 0;
	char *argv[25] = { 0 };
	char *uuid = NULL;
	char *action = NULL;
	switch_media_bug_flag_t flags = SMBF_READ_VIDEO_PING | SMBF_READ_VIDEO_PATCH;
	const char *function = "chromakey";

	if (zstr(cmd)) {
		goto usage;
	}

	if (!(mycmd = strdup(cmd))) {
		goto usage;
	}

	if ((argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])))) < 2) {
		goto usage;
	}

	uuid = argv[0];
	action = argv[1];

	if (!(rsession = switch_core_session_locate(uuid))) {
		stream->write_function(stream, "-ERR Cannot locate session!\n");
		goto done;
	}

	channel = switch_core_session_get_channel(rsession);

	if ((bug = (switch_media_bug_t *) switch_channel_get_private(channel, "_chromakey_bug_"))) {
		if (!zstr(action)) {
			if (!strcasecmp(action, "stop")) {
				switch_channel_set_private(channel, "_chromakey_bug_", NULL);
				switch_core_media_bug_remove(rsession, &bug);
				stream->write_function(stream, "+OK Success\n");
			} else if (!strcasecmp(action, "start")) {
				context = (chromakey_context_t *) switch_core_media_bug_get_user_data(bug);
				switch_assert(context);
				parse_params(context, 2, argc, argv, &function, &flags);
				stream->write_function(stream, "+OK Success\n");
			}
		} else {
			stream->write_function(stream, "-ERR Invalid action\n");
		}
		goto done;
	}

	if (!zstr(action) && strcasecmp(action, "start")) {
		goto usage;
	}

	context = (chromakey_context_t *) switch_core_session_alloc(rsession, sizeof(*context));
	switch_assert(context != NULL);
	context->session = rsession;

	init_context(context);
	parse_params(context, 2, argc, argv, &function, &flags);

	switch_thread_rwlock_rdlock(MODULE_INTERFACE->rwlock);

	if (switch_core_media_bug_add(rsession, function, NULL,
											chromakey_bug_callback, context, 0, flags, &bug) != SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "-ERR Failure!\n");
		switch_thread_rwlock_unlock(MODULE_INTERFACE->rwlock);
		goto done;
	} else {
		switch_channel_set_private(channel, "_chromakey_bug_", bug);
		stream->write_function(stream, "+OK Success\n");
		goto done;
	}

 usage:
	stream->write_function(stream, "-USAGE: %s\n", CHROMAKEY_API_SYNTAX);

 done:
	if (rsession) {
		switch_core_session_rwunlock(rsession);
	}

	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t video_replace_thread_callback(switch_core_session_t *session, switch_frame_t *frame, void *user_data, switch_abc_type_t type)
{
	video_replace_context_t *context = (video_replace_context_t *)user_data;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_frame_t file_frame = { 0 };

	if (!switch_channel_ready(channel)) {
		return SWITCH_STATUS_FALSE;
	}

	if (!frame->img) {
		return SWITCH_STATUS_SUCCESS;
	}

	if (switch_test_flag(&context->vfh, SWITCH_FILE_OPEN)) {
		switch_status_t status = SWITCH_STATUS_FALSE;

		//if (type == SWITCH_ABC_TYPE_READ_VIDEO_PING || (context->vfh.params && switch_true(switch_event_get_header(context->vfh.params, "scale")))) {
			//context->vfh.mm.scale_w = frame->img->d_w;
			//context->vfh.mm.scale_h = frame->img->d_h;
		//}

		status = switch_core_file_read_video(&context->vfh, &file_frame, SVR_FLUSH);
		if (!context->sound) { 
			switch_core_file_command(&context->vfh, SCFC_FLUSH_AUDIO);
		}
		
		if (status != SWITCH_STATUS_SUCCESS && status != SWITCH_STATUS_BREAK) {
			int close = 1;
			
			if (context->vfh.params) {
				const char *loopstr = switch_event_get_header(context->vfh.params, "loop");
				if (switch_true(loopstr)) {
					uint32_t pos = 0;

					switch_core_file_seek(&context->vfh, &pos, 0, SEEK_SET);
					close = 0;
				}
			}

			if (close) {
				switch_core_file_close(&context->vfh);
				switch_core_session_request_video_refresh(session);
			}
		}

		if (file_frame.img) {
			switch_img_free(&(context->rp_img));

			if (frame->img && (frame->img->d_w != file_frame.img->d_w || frame->img->d_h != file_frame.img->d_h)) {
				switch_img_letterbox(file_frame.img, &context->rp_img, frame->img->d_w, frame->img->d_h, "#0000000");
				switch_img_free(&file_frame.img);
			} else {
				context->rp_img = file_frame.img;
			}
		}

		if (context->rp_img) {
			if (context->rp_img->d_w != frame->img->d_w || context->rp_img->d_h != frame->img->d_h ) {
				frame->img = NULL;
			}
			
			switch_img_copy(context->rp_img, &frame->img);
		}
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_bool_t video_replace_bug_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	switch_core_session_t *session = switch_core_media_bug_get_session(bug);
	switch_channel_t *channel = switch_core_session_get_channel(session);
	video_replace_context_t *context = (video_replace_context_t *)user_data;

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
		{
			switch_core_session_request_video_refresh(session);
			switch_channel_set_flag(channel, CF_VIDEO_REFRESH_REQ);
			switch_core_media_gen_key_frame(session);
		}
		break;
	case SWITCH_ABC_TYPE_CLOSE:
		{
			switch_core_session_request_video_refresh(session);
			switch_channel_set_flag(channel, CF_VIDEO_REFRESH_REQ);
			switch_core_media_gen_key_frame(session);
			
			switch_thread_rwlock_unlock(MODULE_INTERFACE->rwlock);
			switch_img_free(&context->rp_img);

			if (switch_test_flag(&context->vfh, SWITCH_FILE_OPEN)) {
				switch_core_file_close(&context->vfh);
				memset(&context->vfh, 0, sizeof(context->vfh));
			}
		}
		break;
	case SWITCH_ABC_TYPE_WRITE_REPLACE:
	case SWITCH_ABC_TYPE_READ_REPLACE:
		{
			switch_frame_t *rframe = NULL;
			switch_size_t len;

			if (type == SWITCH_ABC_TYPE_WRITE_REPLACE) {
				rframe = switch_core_media_bug_get_write_replace_frame(bug);
			} else {
				rframe = switch_core_media_bug_get_read_replace_frame(bug);
			}
			
			if (rframe) {
				len = rframe->samples;

				if (switch_test_flag(&context->vfh, SWITCH_FILE_OPEN)) {
					switch_core_file_read(&context->vfh, rframe->data, &len);

					if (len < rframe->samples) {
						memset((char *)rframe->data + (len * 2 * context->vfh.channels), 0, (rframe->samples - len) * 2 * context->vfh.channels);
					}

					rframe->datalen = rframe->samples * 2 * context->vfh.channels;
				}
			}
		}
		break;
	case SWITCH_ABC_TYPE_READ_VIDEO_PING:
	case SWITCH_ABC_TYPE_WRITE_VIDEO_PING:
		{
			if (switch_test_flag(&context->vfh, SWITCH_FILE_OPEN)) {
				switch_frame_t *frame = switch_core_media_bug_get_video_ping_frame(bug);
				video_replace_thread_callback(context->session, frame, context, type);
			} else {
				switch_channel_set_private(channel, "_video_replace_bug_", NULL);
				return SWITCH_FALSE;
			}
		}
		break;
	default:
		break;
	}

	return SWITCH_TRUE;
}

SWITCH_STANDARD_APP(video_replace_start_function)
{
	switch_media_bug_t *bug;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_flag_t flags = 0;
	const char *function = "video_replace";
	video_replace_context_t *context;
	char *lbuf;
	int argc = 0;
	char *argv[2] = { 0 };
	char *direction = NULL;
	char *file = NULL;
	switch_codec_implementation_t read_impl = { 0 };
	
	if ((bug = (switch_media_bug_t *) switch_channel_get_private(channel, "_video_replace_bug_"))) {
		if (!zstr(data) && !strcasecmp(data, "stop")) {
			switch_channel_set_private(channel, "_video_replace_bug_", NULL);
			switch_core_media_bug_remove(session, &bug);
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "alreday start!\n");
		}
		return;
	}

	if (data && (lbuf = switch_core_session_strdup(session, data))
		&& (argc = switch_separate_string(lbuf, ' ', argv, (sizeof(argv) / sizeof(argv[0])))) > 0) {

		if (argc > 1) {
			direction = argv[0];
			file = argv[1];
		} else {
			direction = "write";
			file = lbuf;
		}

		if (!strcasecmp(direction, "read")) {
			flags = SMBF_READ_VIDEO_PING;
		} else if (!strcasecmp(direction, "write")) {
			flags = SMBF_WRITE_VIDEO_PING;
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "invalid replace direction!\n");
			return;
		}
	} else {
		return;
	}

	// switch_channel_wait_for_flag(channel, CF_VIDEO_READY, SWITCH_TRUE, 10000, NULL);

	context = (video_replace_context_t *) switch_core_session_alloc(session, sizeof(*context));
	switch_assert(context != NULL);
	memset(context, 0, sizeof(*context));
	context->session = session;

	switch_thread_rwlock_rdlock(MODULE_INTERFACE->rwlock);

	switch_core_session_get_read_impl(session, &read_impl);
	
	if (switch_core_file_open(&context->vfh, file, read_impl.number_of_channels, read_impl.actual_samples_per_second,
								  SWITCH_FILE_FLAG_READ | SWITCH_FILE_DATA_SHORT | SWITCH_FILE_FLAG_VIDEO, 
								  switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error opening video file\n");
		switch_thread_rwlock_unlock(MODULE_INTERFACE->rwlock);
		return;
	}

	if (switch_core_media_bug_add(session, function, NULL, video_replace_bug_callback, context, 0, flags, &bug) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failure!\n");
		switch_thread_rwlock_unlock(MODULE_INTERFACE->rwlock);
		return;
	}

	switch_channel_set_private(channel, "_video_replace_bug_", bug);
}

/* API Interface Function */
#define VIDEO_REPLACE_API_SYNTAX "<uuid> <stop|start> [read|write] <file>"
SWITCH_STANDARD_API(video_replace_api_function)
{
	switch_core_session_t *rsession = NULL;
	switch_channel_t *channel = NULL;
	switch_media_bug_t *bug;
	video_replace_context_t *context;
	char *mycmd = NULL;
	int argc = 0;
	char *argv[4] = { 0 };
	char *uuid = NULL;
	char *action = NULL;
	char *file = NULL;
	char *direction = NULL;
	switch_media_bug_flag_t flags = 0;
	const char *function = "video_replace";
	switch_codec_implementation_t read_impl = { 0 };
	int sound = 0;
	
	if (zstr(cmd)) {
		goto usage;
	}

	if (!(mycmd = strdup(cmd))) {
		goto usage;
	}

	if ((argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])))) < 2) {
		goto usage;
	}

	uuid = argv[0];
	action = argv[1];

	if (!(rsession = switch_core_session_locate(uuid))) {
		stream->write_function(stream, "-ERR Cannot locate session!\n");
		goto done;
	}

	channel = switch_core_session_get_channel(rsession);

	bug = (switch_media_bug_t *) switch_channel_get_private(channel, "_video_replace_bug_");

	if (!strcasecmp(action, "stop")) {
		if (bug) {
			switch_channel_set_private(channel, "_video_replace_bug_", NULL);
			switch_core_media_bug_remove(rsession, &bug);
			stream->write_function(stream, "+OK Success\n");
		} else {
			stream->write_function(stream, "-ERR not start\n");
		}

		goto done;
	} else if (!strcasecmp(action, "start")) {
		if (argc == 3) {
			direction = "write";
			file = argv[2];
		} else {
			direction = argv[2];
			file = argv[3];
		}

		if (zstr(direction) || zstr(file)) goto usage;

		if (!strncasecmp(direction, "read", 4)) {
			flags = SMBF_READ_VIDEO_PING;
			if (switch_stristr(":sound", direction)) {
				flags |= SMBF_READ_REPLACE;
				sound = 1;
			}
		} else if (!strncasecmp(direction, "write", 5)) {
			flags = SMBF_WRITE_VIDEO_PING;
			if (switch_stristr(":sound", direction)) {
				flags |= SMBF_WRITE_REPLACE;
				sound = 1;
			}
		} else {
			goto usage;
		}

		if (bug) {
			stream->write_function(stream, "-ERR alreday start\n");
			goto done;
		}
	} else {
		goto usage;
	}

	context = (video_replace_context_t *) switch_core_session_alloc(rsession, sizeof(*context));
	switch_assert(context != NULL);
	context->session = rsession;
	context->sound = sound;
	switch_thread_rwlock_rdlock(MODULE_INTERFACE->rwlock);

	
	switch_core_session_get_read_impl(rsession, &read_impl);
	
	if (switch_core_file_open(&context->vfh, file, read_impl.number_of_channels, read_impl.actual_samples_per_second,
								  SWITCH_FILE_FLAG_READ | SWITCH_FILE_DATA_SHORT | SWITCH_FILE_FLAG_VIDEO, 
								  switch_core_session_get_pool(rsession)) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error opening video file\n");
		switch_thread_rwlock_unlock(MODULE_INTERFACE->rwlock);
		goto done;
	}

	if (switch_core_media_bug_add(rsession, function, NULL,
											video_replace_bug_callback, context, 0, flags, &bug) != SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "-ERR Failure!\n");
		switch_thread_rwlock_unlock(MODULE_INTERFACE->rwlock);
		goto done;
	} else {
		switch_channel_set_private(channel, "_video_replace_bug_", bug);
		stream->write_function(stream, "+OK Success\n");
		goto done;
	}

 usage:
	stream->write_function(stream, "-USAGE: %s\n", VIDEO_REPLACE_API_SYNTAX);

 done:
	if (rsession) {
		switch_core_session_rwunlock(rsession);
	}

	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}


SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_video_filter_shutdown)
{
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_video_filter_load)
{
	switch_application_interface_t *app_interface;
    switch_api_interface_t *api_interface;

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);
	MODULE_INTERFACE = *module_interface;

	SWITCH_ADD_APP(app_interface, "chromakey", "chromakey", "chromakey bug",
				   chromakey_start_function, CHROMAKEY_APP_SYNTAX, SAF_NONE);

    SWITCH_ADD_API(api_interface, "chromakey", "chromakey", chromakey_api_function, CHROMAKEY_API_SYNTAX);

	SWITCH_ADD_APP(app_interface, "video_replace", "video_replace", "video replace bug",
				   video_replace_start_function, "[read|write] <file> | stop", SAF_NONE);

	SWITCH_ADD_API(api_interface, "uuid_video_replace", "video_replace", video_replace_api_function, VIDEO_REPLACE_API_SYNTAX);

    switch_console_set_complete("add chromakey ::console::list_uuid ::[start:stop");
	switch_console_set_complete("add uuid_video_replace ::console::list_uuid start ::[read:write");
	switch_console_set_complete("add uuid_video_replace ::console::list_uuid stop");

	return SWITCH_STATUS_SUCCESS;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4:
 */
