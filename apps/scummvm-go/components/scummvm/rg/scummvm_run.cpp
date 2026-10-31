/* Game detection + engine launch. Same flow as the original johannesbehr/scumm main.cpp,
 * minus the Odroid-Go UI: the Retro-Go launcher now picks the game. GPL-2.0-or-later. */

#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "common/scummsys.h"
#include "common/config-manager.h"
#include "common/fs.h"
#include "common/language.h"
#include "common/platform.h"
#include "common/system.h"
#include "common/textconsole.h"
#include "engines/engine.h"
#include "engines/metaengine.h"
#include "base/commandLine.h"
#include "base/plugins.h"
#include "engines/obsolete.h"
#include "scumm/scummmetaengine.h"
#include "cine/cinemetaengine.h"

#include "osystem_rg.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>

#include "common/unzip.h"
#include "common/stream.h"
#include <unistd.h>

// BEGIN ARCHIVE HELPERS (compiled unchanged by the host-side test harness)

extern "C" void rgx_progress(uint32_t done_kb, uint32_t total_kb, int stage);
extern unsigned long unzGetCurrentUncompressedSize(unzFile file);

// Plain stdio stream. Deliberately NOT Common::FSNode: FSNode(const String&) asserts
// g_system, and the OSystem backend does not exist yet when an archive is opened.
class StdioReadStream : public Common::SeekableReadStream {
public:
	explicit StdioReadStream(FILE *fp) : _fp(fp), _eos(false), _size(0) {
		fseek(_fp, 0, SEEK_END);
		_size = (int32)ftell(_fp);
		fseek(_fp, 0, SEEK_SET);
	}
	~StdioReadStream() { if (_fp) fclose(_fp); }
	bool eos() const { return _eos; }
	bool err() const { return ferror(_fp) != 0; }
	void clearErr() { clearerr(_fp); _eos = false; }
	uint32 read(void *dataPtr, uint32 dataSize) {
		size_t r = fread(dataPtr, 1, dataSize, _fp);
		if (r < dataSize) _eos = true;
		return (uint32)r;
	}
	int32 pos() const { return (int32)ftell(_fp); }
	int32 size() const { return _size; }
	bool seek(int32 offset, int whence = SEEK_SET) {
		_eos = false;
		if (whence == SEEK_SET && offset == (int32)ftell(_fp))
			return true; // avoid dropping the stdio buffer for a no-op seek
		return fseek(_fp, offset, whence) == 0;
	}
private:
	FILE *_fp;
	bool _eos;
	int32 _size;
};

static bool has_ext(const char *path, const char *ext) {
	const char *p = strrchr(path, '.');
	return p && strcasecmp(p, ext) == 0;
}

static const char *base_name(const char *path) {
	const char *p = strrchr(path, '/');
	return p ? p + 1 : path;
}

static void strip_extension(char *name) {
	char *p = strrchr(name, '.');
	if (p) *p = 0;
}

// A ZIP starts with a local header (PK 03 04) or, when empty, an end record (PK 05 06).
static bool has_zip_magic(const char *path) {
	FILE *fp = fopen(path, "rb");
	if (!fp) return false;
	unsigned char m[4] = {0, 0, 0, 0};
	size_t n = fread(m, 1, 4, fp);
	fclose(fp);
	return n == 4 && m[0] == 'P' && m[1] == 'K' &&
	       ((m[2] == 3 && m[3] == 4) || (m[2] == 5 && m[3] == 6));
}

// Any .zip / .scm / .svm file, or any file that really is a ZIP whatever it is called.
static bool is_archive_path(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) return false;
	if (has_ext(path, ".zip") || has_ext(path, ".scm") || has_ext(path, ".svm")) return true;
	return has_zip_magic(path);
}

static bool mkdir_p(const char *path) {
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s", path);
	size_t n = strlen(tmp);
	if (!n) return false;
	if (tmp[n - 1] == '/') tmp[n - 1] = 0;
	for (char *p = tmp + 1; *p; ++p) {
		if (*p == '/') {
			*p = 0;
			mkdir(tmp, 0777);
			*p = '/';
		}
	}
	return mkdir(tmp, 0777) == 0 || errno == EEXIST;
}

// Remove a directory tree (used to clear a stale cache before re-extracting).
static void remove_tree(const char *path, int depth = 0) {
	if (depth > 16) return;
	DIR *d = opendir(path);
	if (!d) { unlink(path); return; }
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
		char child[768];
		snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
		struct stat st;
		if (stat(child, &st) != 0) continue;
		if (S_ISDIR(st.st_mode)) remove_tree(child, depth + 1);
		else unlink(child);
	}
	closedir(d);
	rmdir(path);
}

static bool safe_member_name(const char *name) {
	if (!name || !name[0] || name[0] == '/') return false;
	if (strlen(name) > 1 && name[1] == ':') return false; // drive letter
	const char *p = name;
	while (*p) {
		while (*p == '/') ++p;
		const char *q = p;
		while (*q && *q != '/') ++q;
		size_t len = (size_t)(q - p);
		if (len == 2 && p[0] == '.' && p[1] == '.') return false;
		p = q;
	}
	return true;
}

static bool junk_name(const char *n) {
	return n[0] == '.' || !strcmp(n, "__MACOSX") || !strcmp(n, "Thumbs.db");
}

static bool file_has_size(const char *path, unsigned long size) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode) && (unsigned long)st.st_size == size;
}

// resume: members already on disk with the right size are skipped
static bool extract_zip(unzFile zip, const char *dest, bool resume) {
	if (!zip || !mkdir_p(dest)) return false;

	const unsigned NAME_CAP = 1024;
	char *name = (char *)malloc(NAME_CAP + 1);
	char *out = (char *)malloc(1536);
	char *parent = (char *)malloc(1536);
	unsigned bufSize = 32768;
	uint8_t *buffer = (uint8_t *)malloc(bufSize);
	if (!buffer) { bufSize = 8192; buffer = (uint8_t *)malloc(bufSize); }
	bool ok = false;
	unsigned extracted = 0, entries = 0, skipped = 0;
	unsigned long totalKb = 0, doneKb = 0;

	if (!name || !out || !parent || !buffer) {
		printf("scummvm: out of memory for ZIP extraction\n");
		goto done;
	}

	{
		int err = unzGoToFirstFile(zip);
		if (err != 0) {
			printf("scummvm: ZIP has no readable first entry (%d)\n", err);
			goto done;
		}

		// pass 1: total size, so the progress bar has something to measure against
		rgx_progress(0, 0, 0);
		for (int e = err; e == 0; e = unzGoToNextFile(zip))
			totalKb += (unzGetCurrentUncompressedSize(zip) + 1023) / 1024;
		err = unzGoToFirstFile(zip);
		printf("scummvm: unpacking archive, %lu MB\n", totalKb / 1024);
		rgx_progress(0, (uint32_t)totalKb, 1);

		while (err == 0) {
			++entries;
			memset(name, 0, NAME_CAP + 1);
			if (unzGetCurrentFileInfo(zip, nullptr, name, NAME_CAP, nullptr, 0, nullptr, 0) != 0) {
				printf("scummvm: failed reading ZIP entry %u\n", entries);
				goto done;
			}
			name[NAME_CAP] = 0;
			size_t len = strlen(name);
			if (len == 0 || len >= NAME_CAP) {
				printf("scummvm: refusing oversized/empty ZIP member\n");
				goto done;
			}
			// Some Windows tools write backslashes
			for (char *c = name; *c; ++c) if (*c == '\\') *c = '/';
			if (!safe_member_name(name)) {
				printf("scummvm: refusing unsafe archive member: %s\n", name);
				goto done;
			}

			bool is_dir = name[len - 1] == '/';
			int n = snprintf(out, 1536, "%s/%s", dest, name);
			if (n < 0 || n >= 1536) {
				printf("scummvm: ZIP member path too long: %s\n", name);
				goto done;
			}

			if (is_dir) {
				if (!mkdir_p(out)) goto done;
			} else {
				snprintf(parent, 1536, "%s", out);
				char *slash = strrchr(parent, '/');
				if (slash) {
					*slash = 0;
					if (!mkdir_p(parent)) goto done;
				}
				unsigned long usize = unzGetCurrentUncompressedSize(zip);
				if (resume && file_has_size(out, usize)) {
					++skipped;
					doneKb += (usize + 1023) / 1024;
					rgx_progress((uint32_t)doneKb, (uint32_t)totalKb, 1);
					err = unzGoToNextFile(zip);
					continue;
				}
				if (unzOpenCurrentFile(zip) != 0) {
					printf("scummvm: failed opening ZIP member: %s\n", name);
					goto done;
				}
				FILE *fp = fopen(out, "wb");
				if (!fp) {
					unzCloseCurrentFile(zip);
					printf("scummvm: failed creating extracted file: %s\n", out);
					goto done;
				}
				bool good = true;
				while (true) {
					int got = unzReadCurrentFile(zip, buffer, bufSize);
					if (got < 0) { good = false; break; }
					if (got == 0) break;
					if (fwrite(buffer, 1, (size_t)got, fp) != (size_t)got) { good = false; break; }
					doneKb += (unsigned)got / 1024; // approximate, only drives the bar
					rgx_progress((uint32_t)doneKb, (uint32_t)totalKb, 1);
				}
				fclose(fp);
				int closeErr = unzCloseCurrentFile(zip);
				if (!good || closeErr != 0) {
					printf("scummvm: failed extracting ZIP member: %s\n", name);
					goto done;
				}
				++extracted;
			}
			err = unzGoToNextFile(zip);
		}

		if (err != -100) { // UNZ_END_OF_LIST_OF_FILE
			printf("scummvm: ZIP iteration failed (%d)\n", err);
			goto done;
		}
		printf("scummvm: ZIP contains %u entries, extracted %u files (%u resumed) to %s\n", entries, extracted, skipped, dest);
		ok = true;
	}

done:
	free(name);
	free(out);
	free(parent);
	free(buffer);
	return ok;
}

// Signature of the source archive, stored next to the extracted files so an unchanged
// archive is only unpacked once.
static void archive_signature(const char *zipPath, char *sig, size_t sigSize) {
	struct stat st;
	if (stat(zipPath, &st) != 0) { snprintf(sig, sigSize, "?"); return; }
	snprintf(sig, sigSize, "%ld:%ld", (long)st.st_size, (long)st.st_mtime);
}

static bool cache_is_current(const char *marker, const char *sig) {
	FILE *fp = fopen(marker, "r");
	if (!fp) return false;
	char buf[96] = {0};
	if (!fgets(buf, sizeof(buf), fp)) buf[0] = 0;
	fclose(fp);
	char *nl = strpbrk(buf, "\r\n");
	if (nl) *nl = 0;
	return strcmp(buf, sig) == 0;
}

// Unpack zipPath into cacheRoot (skipped when already unpacked and unchanged).
// 0 = ok, <0 = failure.
static int prepare_archive(const char *zipPath, const char *cacheRoot) {
	char marker[600], sig[96];
	snprintf(marker, sizeof(marker), "%s/.rgzip", cacheRoot);
	archive_signature(zipPath, sig, sizeof(sig));

	if (cache_is_current(marker, sig)) {
		printf("scummvm: reusing extracted files in %s\n", cacheRoot);
		return 0;
	}

	FILE *fp = fopen(zipPath, "rb");
	if (!fp) {
		printf("scummvm: cannot open %s\n", zipPath);
		return -10;
	}
	setvbuf(fp, nullptr, _IOFBF, 16384);
	unzFile zip = unzOpenNoIndex(new StdioReadStream(fp)); // takes ownership of the stream
	if (!zip) {
		printf("scummvm: %s is not a valid ZIP archive\n", zipPath);
		return -10;
	}

	// An interrupted unpack (power off, reset) leaves ".rgzip.part" holding the signature
	// of the archive it belongs to; same archive -> continue, otherwise start clean.
	char part[600];
	snprintf(part, sizeof(part), "%s/.rgzip.part", cacheRoot);
	bool resume = cache_is_current(part, sig);
	if (!resume) {
		remove_tree(cacheRoot); // never mix files from two different archives
		mkdir_p(cacheRoot);
		FILE *pf = fopen(part, "w");
		if (pf) { fprintf(pf, "%s\n", sig); fclose(pf); }
	} else {
		printf("scummvm: resuming interrupted unpack\n");
	}
	bool ok = extract_zip(zip, cacheRoot, resume);
	unzClose(zip);
	if (!ok) {
		printf("scummvm: archive extraction failed\n");
		return -11;
	}

	FILE *mk = fopen(marker, "w"); // written last: a half-extracted cache is never trusted
	if (mk) {
		fprintf(mk, "%s\n", sig);
		fclose(mk);
	}
	remove(part);
	return 0;
}

// Lists the single real sub-folder of root, if there is exactly one.
static bool single_subdir(const char *root, char *out, size_t outSize) {
	DIR *d = opendir(root);
	if (!d) return false;
	int dirs = 0;
	char only[768] = {0};
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..") || junk_name(ent->d_name)) continue;
		char path[768];
		snprintf(path, sizeof(path), "%s/%s", root, ent->d_name);
		struct stat st;
		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
			++dirs;
			snprintf(only, sizeof(only), "%s", path);
		}
	}
	closedir(d);
	if (dirs != 1) return false;
	snprintf(out, outSize, "%s", only);
	return true;
}

// END ARCHIVE HELPERS

extern "C" int scummvm_run(const char *gamePath, const char *gameIdHint) {
	bool archiveMode = is_archive_path(gamePath);
	Common::String dir(gamePath);
	char extractedRoot[512] = {0};
	char saveRoot[512] = {0};

	if (archiveMode) {
		char gameName[256];
		snprintf(gameName, sizeof(gameName), "%s", base_name(gamePath));
		strip_extension(gameName);

		// Archives are unpacked outside the ROM folder (launcher stays clean) into a
		// normal writable tree, which is also where saves and scummvm.ini live.
		snprintf(extractedRoot, sizeof(extractedRoot), "/sd/retro-go/cache/scummvm/%s", gameName);
		snprintf(saveRoot, sizeof(saveRoot), "/sd/retro-go/saves/scummvm/%s", gameName);
		mkdir_p("/sd/retro-go/cache/scummvm");
		mkdir_p(saveRoot);

		int rc = prepare_archive(gamePath, extractedRoot);
		if (rc != 0)
			return rc;
		dir = extractedRoot;
	}

	// Use a guaranteed-valid internal configuration domain.
	Common::String target = "scummvm_game";
	if (gameIdHint && gameIdHint[0])
		target = Common::String("scummvm_") + gameIdHint;

	Common::String saveDir;
	Common::String configFile;
	if (archiveMode) {
		saveDir = saveRoot;
		configFile = Common::String(saveRoot) + "/scummvm.ini";
	} else {
		saveDir = dir + "/saves";
		configFile = dir + "/scummvm.ini";
		mkdir(saveDir.c_str(), 0777);
	}

	RgBackend::OSystem_RG *system = new RgBackend::OSystem_RG(saveDir.c_str(), configFile.c_str());
	g_system = system; // from here on Common::FSNode and friends are safe to use

	Base::registerDefaults();
	ConfMan.loadDefaultConfigFile();
	ConfMan.setBool("subtitles", true);

	PluginManager::instance().init();
	PluginManager::instance().loadAllPlugins();
	system->initBackend();

	ConfMan.addGameDomain(target);
	ConfMan.set("path", dir, target);
	ConfMan.set("language", Common::getLanguageCode(Common::UNK_LANG), target);
	ConfMan.set("platform", Common::getPlatformCode(Common::kPlatformUnknown), target);
	ConfMan.setActiveDomain(target);

	Cine::CineMetaEngine cineEngine;
	Scumm::ScummMetaEngine scummEngine;
	MetaEngine *engines[] = { &scummEngine, &cineEngine };

	// Folder games: detect in the folder. Archives: detect in the unpacked root and, if a
	// zip wraps everything in one top-level folder, in that folder.
	Common::String candidates[2];
	int numCandidates = 0;
	candidates[numCandidates++] = dir;
	if (archiveMode) {
		char sub[768];
		if (single_subdir(extractedRoot, sub, sizeof(sub)))
			candidates[numCandidates++] = sub;
	}

	MetaEngine *meta = nullptr;
	DetectedGames games;
	for (int c = 0; c < numCandidates && games.empty(); c++) {
		Common::FSNode dirNode(candidates[c]);
		Common::FSList files;
		dirNode.getChildren(files, Common::FSNode::kListAll);
		printf("scummvm: %d files in %s\n", (int)files.size(), candidates[c].c_str());
		for (int i = 0; i < 2; i++) {
			meta = engines[i];
			games = meta->detectGames(files);
			if (!games.empty()) {
				dir = candidates[c];
				break;
			}
		}
	}

	if (games.empty()) {
		printf("scummvm: no supported game found in %s\n", dir.c_str());
		system->quit();
		return -1;
	}
	ConfMan.set("path", dir, target); // the folder where the game really is

	uint pick = 0;
	if (gameIdHint && gameIdHint[0]) {
		for (uint i = 0; i < games.size(); i++) {
			if (games[i].gameId == gameIdHint) {
				pick = i;
				break;
			}
		}
	}
	printf("scummvm: gameid %s\n", games[pick].gameId.c_str());
	ConfMan.set("gameid", games[pick].gameId, target);

	Engine *engine = nullptr;
	Common::Error err = meta->createInstance(g_system, &engine);
	if (err.getCode() != Common::kNoError || !engine) {
		printf("scummvm: createInstance failed (%d)\n", (int)err.getCode());
		system->quit();
		return -2;
	}

	g_system->engineInit();
	Common::Error result = engine->run();
	printf("scummvm: engine finished (%d)\n", (int)result.getCode());
	delete engine;
	g_system->engineDone();
	system->quit();
	return (int)result.getCode();
}
