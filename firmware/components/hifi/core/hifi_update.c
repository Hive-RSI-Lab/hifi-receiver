#include <stdio.h>
#include <string.h>

#include "hifi_json.h"
#include "hifi_update.h"

const char *hifi_update_state_name(hifi_upd_state_t s) {
	switch (s) {
	case HIFI_UPD_IDLE: return "idle";
	case HIFI_UPD_DOWNLOADING: return "downloading";
	case HIFI_UPD_FLASHING: return "flashing";
	case HIFI_UPD_DONE: return "done";
	case HIFI_UPD_ERROR: return "error";
	}
	return "idle";
}

static bool is_hex64(const char *s) {
	if (strlen(s) != 64) return false;
	for (; *s; s++)
		if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F'))) return false;
	return true;
}

/* url and sha256 from one image object (or the manifest's top level) */
static bool image(const char *obj, char *url, size_t ucap, char *sha, size_t scap) {
	if (!hj_get_str(obj, "url", url, ucap) || !url[0]) return false;
	if (!hj_get_str(obj, "sha256", sha, scap)) sha[0] = '\0';
	return true;
}

int hifi_update_parse(const char *manifest, const char *manifest_url, const char *model,
		      hifi_update_offer_t *out, const char **err) {
	memset(out, 0, sizeof(*out));
	if (!manifest || !hj_valid_object(manifest)) return *err = "the update manifest isn't a JSON object", -1;
	if (!hj_get_str(manifest, "latest", out->latest, sizeof(out->latest)) || !out->latest[0])
		return *err = "the update manifest has no latest version", -1;
	hj_get_str(manifest, "notes", out->notes, sizeof(out->notes));

	char url[sizeof(out->url)] = "";
	bool found = false;
	const char *imgs, *one;
	size_t n, m;
	if (hj_get_raw(manifest, "images", &imgs, &n) && n < 4096 && imgs[0] == '{') {
		char buf[4096];
		memcpy(buf, imgs, n);
		buf[n] = '\0';
		if (hj_get_raw(buf, model, &one, &m) && one[0] == '{' && m < 1024) {
			char obj[1024];
			memcpy(obj, one, m);
			obj[m] = '\0';
			found = image(obj, url, sizeof(url), out->sha256, sizeof(out->sha256));
		}
	}
	if (!found) found = image(manifest, url, sizeof(url), out->sha256, sizeof(out->sha256));
	if (!found) return *err = "the update manifest has no image for this model", -1;
	if (out->sha256[0] && !is_hex64(out->sha256)) return *err = "the update manifest's sha256 isn't 64 hex digits", -1;

	size_t keep = 0;
	const char *sep = "";
	if (!strstr(url, "://") && manifest_url) {
		/* relative: from the manifest's folder, or its host's root for "/x" */
		const char *scheme = strstr(manifest_url, "://");
		const char *path = scheme ? strchr(scheme + 3, '/') : NULL;
		if (url[0] == '/') keep = path ? (size_t) (path - manifest_url) : strlen(manifest_url);
		else if (path) keep = (size_t) (strrchr(path, '/') - manifest_url) + 1;
		else keep = strlen(manifest_url), sep = "/";
	}
	if (snprintf(out->url, sizeof(out->url), "%.*s%s%s", (int) keep, manifest_url ? manifest_url : "", sep, url) >=
	    (int) sizeof(out->url))
		return *err = "the image's url is too long", -1;
	return 0;
}
