/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>
                          Zachary Lund <admin@computerquip.com>
                          Philippe Groarke <philippe.groarke@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "OBSBasic.hpp"

#include <utility/RecordingSize.hpp>
#include <qt-wrappers.hpp>

#include <QDir>

namespace {
bool RestoreRecordingItem(obs_sceneitem_t *item);

struct RecordingSizeSelection {
	obs_sceneitem_t *keep = nullptr;
	bool changed = false;
};

bool ClearOtherRecordingSizes(obs_scene_t *, obs_sceneitem_t *item, void *data)
{
	auto &selection = *static_cast<RecordingSizeSelection *>(data);
	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, ClearOtherRecordingSizes, data);
	}
	if (item != selection.keep) {
		OBSDataAutoRelease settings = obs_sceneitem_get_private_settings(item);
		if (obs_data_get_int(settings, "recording_size_mode")) {
			obs_data_erase(settings, "recording_size_mode");
			selection.changed = true;
		}
		selection.changed = RestoreRecordingItem(item) || selection.changed;
	}
	return true;
}

OBSSceneItem RecordingSizeItem(obs_scene_t *scene, bool &changed)
{
	struct Search {
		OBSSceneItem result;
		const char *sceneUuid;
		bool original = false;
	} search = {{}, obs_source_get_uuid(obs_scene_get_source(scene))};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *data) {
			auto &search = *static_cast<Search *>(data);
			OBSDataAutoRelease settings = obs_sceneitem_get_private_settings(item);
			int mode = (int)obs_data_get_int(settings, "recording_size_mode");
			if (!obs_sceneitem_is_group(item) && (mode == 1 || mode == 2)) {
				bool original = strcmp(obs_data_get_string(settings, "recording_size_scene"),
						       search.sceneUuid) == 0 &&
						obs_data_get_int(settings, "recording_size_original_item") ==
							obs_sceneitem_get_id(item);
				if (!search.result || (original && !search.original)) {
					search.result = item;
					search.original = original;
				}
			}
			return true;
		},
		&search);
	RecordingSizeSelection selection = {search.result};
	obs_scene_enum_items(scene, ClearOtherRecordingSizes, &selection);
	changed = selection.changed;
	if (search.result && !search.original) {
		OBSDataAutoRelease settings = obs_sceneitem_get_private_settings(search.result);
		obs_data_set_string(settings, "recording_size_scene", search.sceneUuid);
		obs_data_set_int(settings, "recording_size_original_item", obs_sceneitem_get_id(search.result));
		changed = true;
	}
	return search.result;
}

bool RecordingCrop(obs_sceneitem_t *item, int mode, obs_sceneitem_crop &crop, QString &error)
{
	crop = {};
	if (mode != 2) {
		return true;
	}
	obs_source_t *source = obs_sceneitem_get_source(item);
	// A filter can change the geometry after the capture plugin reports its region.
	if (obs_source_get_width(source) != obs_source_get_base_width(source) ||
	    obs_source_get_height(source) != obs_source_get_base_height(source)) {
		error = QTStr("RecordingSize.FilteredSource");
		return false;
	}
	calldata_t data = {};
	bool called = proc_handler_call(obs_source_get_proc_handler(source), "get_chrome_page_region", &data);
	bool valid = called && calldata_bool(&data, "valid");
	if (valid) {
		crop.left = (int)calldata_int(&data, "left");
		crop.top = (int)calldata_int(&data, "top");
		crop.right = (int)calldata_int(&data, "right");
		crop.bottom = (int)calldata_int(&data, "bottom");
	} else {
		const char *reason = calldata_string(&data, "error");
		if (reason && strcmp(reason, "permission_required") == 0) {
			error = QTStr("RecordingSize.AccessibilityRequired");
		} else if (!called || !calldata_bool(&data, "supported")) {
			error = QTStr("RecordingSize.ChromeRequired");
		} else {
			error = QTStr("RecordingSize.PageUnavailable");
		}
	}
	calldata_free(&data);
	return valid;
}

bool RestoreRecordingItem(obs_sceneitem_t *item)
{
	OBSDataAutoRelease settings = obs_sceneitem_get_private_settings(item);
	if (!obs_data_get_bool(settings, "recording_size_hidden")) {
		return false;
	}
	obs_data_erase(settings, "recording_size_hidden");
	obs_sceneitem_set_visible(item, true);
	return true;
}

void HideUnavailablePage(obs_sceneitem_t *item)
{
	OBSSource showTransition = obs_sceneitem_get_transition(item, true);
	OBSSource hideTransition = obs_sceneitem_get_transition(item, false);
	if (showTransition) {
		obs_transition_force_stop(showTransition);
	}
	if (hideTransition) {
		obs_transition_force_stop(hideTransition);
	}
	if (obs_sceneitem_visible(item)) {
		OBSDataAutoRelease settings = obs_sceneitem_get_private_settings(item);
		obs_data_set_bool(settings, "recording_size_hidden", true);
		// An animated hide would continue displaying the stale browser crop.
		obs_sceneitem_set_transition(item, false, nullptr);
		obs_sceneitem_set_visible(item, false);
		obs_sceneitem_set_transition(item, false, hideTransition);
	}
}

bool FitRecordingItem(obs_sceneitem_t *item, const obs_sceneitem_crop &crop, uint32_t width, uint32_t height)
{
	obs_sceneitem_crop previousCrop;
	obs_sceneitem_get_crop(item, &previousCrop);
	obs_transform_info previous;
	obs_sceneitem_get_info2(item, &previous);
	bool changed = previousCrop.left != crop.left || previousCrop.top != crop.top ||
		       previousCrop.right != crop.right || previousCrop.bottom != crop.bottom || previous.pos.x != 0 ||
		       previous.pos.y != 0 || previous.rot != 0 || previous.scale.x != 1 || previous.scale.y != 1 ||
		       previous.alignment != (OBS_ALIGN_LEFT | OBS_ALIGN_TOP) ||
		       previous.bounds_type != OBS_BOUNDS_SCALE_INNER || previous.bounds.x != float(width) ||
		       previous.bounds.y != float(height) || previous.bounds_alignment != OBS_ALIGN_CENTER ||
		       previous.crop_to_bounds;
	if (changed) {
		obs_transform_info info = {};
		vec2_set(&info.scale, 1, 1);
		info.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
		info.bounds_type = OBS_BOUNDS_SCALE_INNER;
		info.bounds_alignment = OBS_ALIGN_CENTER;
		vec2_set(&info.bounds, float(width), float(height));
		obs_sceneitem_defer_update_begin(item);
		obs_sceneitem_set_crop(item, &crop);
		obs_sceneitem_set_info2(item, &info);
		obs_sceneitem_defer_update_end(item);
	}
	return RestoreRecordingItem(item) || changed;
}
} // namespace

void OBSBasic::SetRecordingSizeSource(OBSSceneItem item, int mode)
{
	if (!item || Active() || obs_video_active() || streamingStarting || disableSaving || isClosing()) {
		return;
	}
	OBSScene scene = GetCurrentScene();
	if (obs_sceneitem_get_scene(item) != scene || obs_sceneitem_is_group(item)) {
		return;
	}
	if (mode == 2) {
		calldata_t data = {};
		proc_handler_call(obs_source_get_proc_handler(obs_sceneitem_get_source(item)),
				  "request_chrome_page_access", &data);
		calldata_free(&data);
	}
	obs_sceneitem_crop crop;
	QString error;
	if (mode && !RecordingCrop(item, mode, crop, error)) {
		OBSMessageBox::warning(this, QTStr("RecordingSize"), error);
		return;
	}
	// Item private settings are copied with items, including Studio Mode's private scenes.
	RecordingSizeSelection selection;
	obs_scene_enum_items(scene, ClearOtherRecordingSizes, &selection);
	OBSDataAutoRelease settings = obs_sceneitem_get_private_settings(item);
	obs_data_set_int(settings, "recording_size_mode", mode);
	obs_data_set_string(settings, "recording_size_scene", obs_source_get_uuid(obs_scene_get_source(scene)));
	obs_data_set_int(settings, "recording_size_original_item", obs_sceneitem_get_id(item));
	pendingRecordingSize.reset();
	UpdateRecordingSize(true);
	SaveProject();
}

bool OBSBasic::UpdateRecordingSize(bool notify)
{
	if (disableSaving || isClosing() || !activeConfiguration || !outputHandler) {
		return true;
	}
	OBSSourceAutoRelease output = obs_get_output_source(0);
	OBSSourceAutoRelease sourceOnAir = output && obs_source_get_type(output) == OBS_SOURCE_TYPE_TRANSITION
						   ? obs_transition_get_active_source(output)
						   : obs_source_get_ref(output);
	OBSScene scene = obs_scene_from_source(sourceOnAir);
	if (!scene) {
		return true;
	}
	bool selectionChanged = false;
	OBSSceneItem item = RecordingSizeItem(scene, selectionChanged);
	if (selectionChanged) {
		SaveProject();
	}
	if (!item) {
		pendingRecordingSize.reset();
		return true;
	}
	OBSDataAutoRelease settings = obs_sceneitem_get_private_settings(item);
	int mode = (int)obs_data_get_int(settings, "recording_size_mode");
	auto fail = [&](const QString &message) {
		if (mode == 2) {
			HideUnavailablePage(item);
		}
		if (notify) {
			OBSMessageBox::warning(this, QTStr("RecordingSize"), message);
		}
		return false;
	};
	obs_sceneitem_crop crop;
	QString error;
	if (!RecordingCrop(item, mode, crop, error)) {
		return fail(error);
	}
	obs_source_t *source = obs_sceneitem_get_source(item);
	auto size = MatchedRecordingSize(obs_source_get_width(source), obs_source_get_height(source), crop.left,
					 crop.top, crop.right, crop.bottom);
	if (!size) {
		return fail(QTStr("RecordingSize.SourceUnavailable"));
	}
	obs_video_info video;
	if (!obs_get_video_info(&video)) {
		return fail(QTStr("RecordingSize.SourceUnavailable"));
	}
	// Outputs keep their initial dimensions; keep the source fitted inside that frame.
	bool transitioning = output && obs_source_get_type(output) == OBS_SOURCE_TYPE_TRANSITION &&
			     obs_transition_get_time(output) > 0 && obs_transition_get_time(output) < 1;
	bool busy = Active() || obs_video_active() || streamingStarting || transitioning ||
		    (setupStreamingGuard.valid() &&
		     setupStreamingGuard.wait_for(std::chrono::seconds{0}) != std::future_status::ready);
	bool settled = notify || pendingRecordingSize == size;
	pendingRecordingSize = size;
	if (busy || !settled) {
		if (FitRecordingItem(item, crop, video.base_width, video.base_height)) {
			SaveProject();
		}
		return true;
	}
	auto [width, height] = *size;
	bool resize = video.base_width != width || video.base_height != height || video.output_width != width ||
		      video.output_height != height;
	if (resize) {
		config_set_uint(activeConfiguration, "Video", "BaseCX", width);
		config_set_uint(activeConfiguration, "Video", "BaseCY", height);
		config_set_uint(activeConfiguration, "Video", "OutputCX", width);
		config_set_uint(activeConfiguration, "Video", "OutputCY", height);
		if (ResetVideo() != OBS_VIDEO_SUCCESS) {
			config_set_uint(activeConfiguration, "Video", "BaseCX", video.base_width);
			config_set_uint(activeConfiguration, "Video", "BaseCY", video.base_height);
			config_set_uint(activeConfiguration, "Video", "OutputCX", video.output_width);
			config_set_uint(activeConfiguration, "Video", "OutputCY", video.output_height);
			ResetVideo();
			return fail(QTStr("RecordingSize.ResetFailed"));
		}
	}
	bool rescaleChanged = false;
	if (strcmp(config_get_string(activeConfiguration, "Output", "Mode"), "Advanced") == 0) {
		if (strcmp(config_get_string(activeConfiguration, "AdvOut", "RecType"), "FFmpeg") == 0) {
			rescaleChanged = config_get_bool(activeConfiguration, "AdvOut", "FFRescale");
			config_set_bool(activeConfiguration, "AdvOut", "FFRescale", false);
		} else {
			bool shared = strcmp(config_get_string(activeConfiguration, "AdvOut", "RecEncoder"), "none") ==
				      0;
			const char *key = shared ? "RescaleFilter" : "RecRescaleFilter";
			rescaleChanged = config_get_int(activeConfiguration, "AdvOut", key) != OBS_SCALE_DISABLE;
			config_set_int(activeConfiguration, "AdvOut", key, OBS_SCALE_DISABLE);
		}
	}
	if (resize || rescaleChanged) {
		ResetOutputs();
		activeConfiguration.SaveSafe("tmp");
	}
	if (FitRecordingItem(item, crop, width, height)) {
		SaveProject();
	}
	return true;
}

void OBSBasic::ResetOutputs()
{
	ProfileScope("OBSBasic::ResetOutputs");

	const char *mode = config_get_string(activeConfiguration, "Output", "Mode");
	bool advOut = astrcmpi(mode, "Advanced") == 0;

	if ((!outputHandler || !outputHandler->Active()) &&
	    (!setupStreamingGuard.valid() ||
	     setupStreamingGuard.wait_for(std::chrono::seconds{0}) == std::future_status::ready)) {
		outputHandler.reset();
		outputHandler.reset(advOut ? CreateAdvancedOutputHandler(this) : CreateSimpleOutputHandler(this));

		emit ReplayBufEnabled(outputHandler->replayBuffer);

		if (sysTrayReplayBuffer) {
			sysTrayReplayBuffer->setEnabled(!!outputHandler->replayBuffer);
		}

		UpdateIsRecordingPausable();
	} else {
		outputHandler->Update();
	}
}

bool OBSBasic::Active() const
{
	if (!outputHandler) {
		return false;
	}
	return outputHandler->Active();
}

void OBSBasic::ResizeOutputSizeOfSource()
{
	if (obs_video_active()) {
		return;
	}

	QMessageBox resize_output(this);
	resize_output.setText(QTStr("ResizeOutputSizeOfSource.Text") + "\n\n" +
			      QTStr("ResizeOutputSizeOfSource.Continue"));
	QAbstractButton *Yes = resize_output.addButton(QTStr("Yes"), QMessageBox::YesRole);
	resize_output.addButton(QTStr("No"), QMessageBox::NoRole);
	resize_output.setIcon(QMessageBox::Warning);
	resize_output.setWindowTitle(QTStr("ResizeOutputSizeOfSource"));
	resize_output.exec();

	if (resize_output.clickedButton() != Yes) {
		return;
	}

	OBSSource source = obs_sceneitem_get_source(GetCurrentSceneItem());

	int width = obs_source_get_width(source);
	int height = obs_source_get_height(source);

	width = ((width + 3) / 4) * 4;   // Round width up to the nearest multiple of 4
	height = ((height + 1) / 2) * 2; // Round height up to the nearest multiple of 2

	config_set_uint(activeConfiguration, "Video", "BaseCX", width);
	config_set_uint(activeConfiguration, "Video", "BaseCY", height);
	config_set_uint(activeConfiguration, "Video", "OutputCX", width);
	config_set_uint(activeConfiguration, "Video", "OutputCY", height);

	ResetVideo();
	ResetOutputs();
	activeConfiguration.SaveSafe("tmp");
	on_actionFitToScreen_triggered();
}

const char *OBSBasic::GetCurrentOutputPath()
{
	const char *path = nullptr;
	const char *mode = config_get_string(Config(), "Output", "Mode");

	if (strcmp(mode, "Advanced") == 0) {
		const char *advanced_mode = config_get_string(Config(), "AdvOut", "RecType");

		if (strcmp(advanced_mode, "FFmpeg") == 0) {
			path = config_get_string(Config(), "AdvOut", "FFFilePath");
		} else {
			path = config_get_string(Config(), "AdvOut", "RecFilePath");
		}
	} else {
		path = config_get_string(Config(), "SimpleOutput", "FilePath");
	}

	return path;
}

void OBSBasic::OutputPathInvalidMessage()
{
	blog(LOG_ERROR, "Recording stopped because of bad output path");

	OBSMessageBox::critical(this, QTStr("Output.BadPath.Title"), QTStr("Output.BadPath.Text"));
}

bool OBSBasic::IsFFmpegOutputToURL() const
{
	const char *mode = config_get_string(Config(), "Output", "Mode");
	if (strcmp(mode, "Advanced") == 0) {
		const char *advanced_mode = config_get_string(Config(), "AdvOut", "RecType");
		if (strcmp(advanced_mode, "FFmpeg") == 0) {
			bool is_local = config_get_bool(Config(), "AdvOut", "FFOutputToFile");
			if (!is_local) {
				return true;
			}
		}
	}

	return false;
}

bool OBSBasic::OutputPathValid()
{
	if (IsFFmpegOutputToURL()) {
		return true;
	}

	const char *path = GetCurrentOutputPath();
	return path && *path && QDir(path).exists();
}
