// Lingo libretro front end: speech to text for voiced lines, offline, with
// whisper.cpp (MIT) and an OpenAI Whisper model (MIT) converted to ggml.
//
// The model is app0:lingo/whisper.bin (the small "tiny" model, shipped), or
// the largest .bin in ux0:data/lingo/models/ (a bigger one sent from a
// phone recognises more but takes longer).
#include "retro-whisper.h"

#include "whisper.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include <vector>

static whisper_context* sCtx;
static std::string sModel;

static std::string pickModel() {
	std::string best;
	long bestSize = 0;
	DIR* d = opendir("ux0:data/lingo/models");
	if (d) {
		while (dirent* e = readdir(d)) {
			std::string name = e->d_name;
			if (name.size() < 5 || name.substr(name.size() - 4) != ".bin") {
				continue;
			}
			std::string path = "ux0:data/lingo/models/" + name;
			FILE* f = fopen(path.c_str(), "rb");
			if (!f) {
				continue;
			}
			fseek(f, 0, SEEK_END);
			long size = ftell(f);
			fclose(f);
			if (size > bestSize) {
				bestSize = size;
				best = path;
			}
		}
		closedir(d);
	}
	if (best.empty()) {
		FILE* f = fopen("app0:lingo/whisper.bin", "rb");
		if (f) {
			fclose(f);
			best = "app0:lingo/whisper.bin";
		}
	}
	return best;
}

extern "C" bool lingoWhisperAvailable(void) {
	return sCtx || !pickModel().empty();
}

extern "C" const char* lingoWhisperModel(void) {
	return sModel.c_str();
}

struct Progress {
	volatile int* out;
};

static void onProgress(whisper_context*, whisper_state*, int progress, void* user) {
	Progress* p = (Progress*) user;
	if (p->out) {
		*p->out = progress;
	}
}

extern "C" int lingoWhisperTranscribe(const int16_t* pcm16k, int n, char* out, int outLen, volatile int* progress) {
	out[0] = 0;
	if (!sCtx) {
		sModel = pickModel();
		if (sModel.empty()) {
			snprintf(out, outLen, "No speech model found.");
			return -1;
		}
		whisper_context_params cp = whisper_context_default_params();
		cp.use_gpu = false;
		sCtx = whisper_init_from_file_with_params(sModel.c_str(), cp);
		if (!sCtx) {
			snprintf(out, outLen, "Couldn't load the speech model (%s).", sModel.c_str());
			return -1;
		}
	}
	std::vector<float> pcm(n);
	for (int i = 0; i < n; ++i) {
		pcm[i] = pcm16k[i] / 32768.0f;
	}
	whisper_full_params p = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
	p.n_threads = 3; /* the Vita gives a game three cores */
	p.language = "ja";
	p.translate = false;
	p.no_context = true;
	p.no_timestamps = true;
	p.single_segment = true;
	p.print_progress = false;
	p.print_realtime = false;
	p.print_timestamps = false;
	p.suppress_blank = true;
	p.suppress_non_speech_tokens = true;
	p.temperature_inc = 0; /* no slow fallback passes */
	p.greedy.best_of = 1;
	/* encode only as much audio as there is (the model's window is 30 s) */
	int ctx = (int) ((double) n / 16000 * 50) + 64;
	p.audio_ctx = ctx < 1500 ? (ctx < 256 ? 256 : ctx) : 0;
	Progress prog = { progress };
	p.progress_callback = onProgress;
	p.progress_callback_user_data = &prog;
	if (whisper_full(sCtx, p, pcm.data(), n) != 0) {
		snprintf(out, outLen, "Speech recognition failed.");
		return -1;
	}
	std::string text;
	int segs = whisper_full_n_segments(sCtx);
	for (int i = 0; i < segs; ++i) {
		text += whisper_full_get_segment_text(sCtx, i);
	}
	/* trim spaces */
	size_t a = text.find_first_not_of(" \t\n");
	size_t b = text.find_last_not_of(" \t\n");
	text = a == std::string::npos ? "" : text.substr(a, b - a + 1);
	snprintf(out, outLen, "%s", text.c_str());
	return (int) text.size();
}

extern "C" void lingoWhisperUnload(void) {
	if (sCtx) {
		whisper_free(sCtx);
		sCtx = nullptr;
	}
}
