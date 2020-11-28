#include <file-updater/file-updater.h>
#include <util/threading.h>
#include <util/platform.h>
#include <obs-module.h>
#include <util/dstr.h>
#include <jansson.h>

#include <pthread.h>

#include "onlyfans.h"
#include "check-connection.h"

typedef struct onlyfans_ingest onlyfans_ingest_t;

static update_info_t *onlyfans_update_info = NULL;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static bool ingests_refreshed = false;
static bool ingests_refreshing = false;
static bool ingests_loaded = false;
static bool update_stopped = false;
static pthread_t thread;
//!< Keeps default server.
static const char * default_server = "rtmp://route0.onlyfans.com/live";
//!< Keeps default a name of ingests file.
static const char * default_ingests_file_name = "onlyfans_ingests.json";
static const char * default_ingests_temp_file_name = "onlyfans_ingests.temp.json";
//!< Keeps a destination to get a list of ingests.
static const char * destination_url = "https://vh0.devel.retloko.dev/api/v1/ingests";
//!< Keeps a default port.
static const long default_port = 1935;
// Keeps update timeout (in seconds).
static const uint32_t update_timeout = 10;

extern const char *get_module_name(void);

struct ingest {
	char *name;
	char *url;
	int rtt;
};
typedef struct ingest ingest_t;

static DARRAY(struct ingest) cur_ingests;

static void free_ingests(void) {
	for (size_t i = 0; i < cur_ingests.num; i++) {
                ingest_t *ingest = (ingest_t *)(cur_ingests.array + i);
		bfree(ingest->name);
		bfree(ingest->url);
	}
	da_free(cur_ingests);
}

//!< Sorts a list of ingests by RTT.
static void bubble_sort_by_rtt(ingest_t * data, size_t size) {
        ingest_t tmp;

        for (size_t i = 0; i < size; ++i) {
                for (size_t j = size - 1; j >= i + 1; --j) {
                        if (data[j].rtt < data[j - 1].rtt) {
                                tmp = data[j];
                                data[j] = data[j - 1];
                                data[j - 1] = tmp;
                        }
                }
        }
}

static json_t * build_object(const ingest_t * ingest) {
        json_t *object = json_object();
        if (object != NULL) {
                json_object_set(object, "name", json_string(ingest->name));
                json_object_set(object, "url", json_string(ingest->url));
                json_object_set(object, "rtt", json_integer(ingest->rtt));
        }
        return object;
}

static void dump_cache(const json_t *json, const char *file_name) {
        char * onlyfans_cache = obs_module_config_path(file_name);
        if (onlyfans_cache) {
                char *dump = json_dumps(json, JSON_COMPACT);
		if (dump != NULL) {
                        char * cache_new = obs_module_config_path(default_ingests_temp_file_name);
                        os_quick_write_utf8_file(cache_new,
                                                 dump,
                                                 strlen(dump),
                                                 false);
                        os_safe_replace(onlyfans_cache,
                                        cache_new,
                                        NULL);
                        bfree(cache_new);
                        bfree(dump);
		}
		// Releasing allocated memory.
		bfree(onlyfans_cache);
	}
}

static void save_cache_ingests() {
	const size_t ingest_count = onlyfans_ingest_count();
	if (ingest_count > 0) {
                json_t *root = json_object();
                json_t *ingests = json_array();

                for (size_t i = 0; i < ingest_count; ++i) {
                        const ingest_t *ingest = cur_ingests.array + i;
                        // Adding a new ingest.
                        json_t *object = build_object(ingest);
                        if (object != NULL) {
                                // Appending a new ingest.
                                json_array_append(ingests, object);
                                json_decref(object);
                        }
                }
                // Appending the array into root object.
                json_object_set(root, "ingests", ingests);
		// Dumping the cache into a file.
		dump_cache(root, default_ingests_file_name);

                json_decref(ingests);
                json_decref(root);
	}
}

static bool load_ingests(const char *json, bool write_file) {
	json_t *root;
	json_t *ingests;
	bool success = false;
	size_t count = 0;

	root = json_loads(json, 0, NULL);
	if (!root) {
		goto finish;
	}
	ingests = json_object_get(root, "ingests");
	if (!ingests) {
		goto finish;
	}
	count = json_array_size(ingests);
	if (count <= 1 && cur_ingests.num) {
		goto finish;
	}
	free_ingests();

	for (size_t i = 0; i < count; i++) {
		json_t *item = json_array_get(ingests, i);
		json_t *item_name = json_object_get(item, "name");
		json_t *item_url = json_object_get(item, "url");
		json_t *item_rtt = json_object_get(item, "rtt");
		ingest_t ingest = {0};
		struct dstr url = {0};

		if (!item_name || !item_url) {
			continue;
		}
		const char *url_str = json_string_value(item_url);
		const char *name_str = json_string_value(item_name);
                ingest.rtt = (item_rtt != NULL) ? json_integer_value(item_rtt)
						: INT32_MAX;
		dstr_copy(&url, url_str);

		ingest.name = bstrdup(name_str);
		ingest.url = url.array;

		da_push_back(cur_ingests, &ingest);
	}

	if (!cur_ingests.num) {
		goto finish;
	}
	success = true;

	if (!write_file) {
		goto finish;
	}
	// Saving a list of ingests.
        save_cache_ingests();

finish:
	if (root) {
		json_decref(root);
	}
	return success;
}

static bool onlyfans_ingest_update(void *param, struct file_download_data *data) {
	bool success = false;

	pthread_mutex_lock(&mutex);
	success = load_ingests((const char *)data->buffer.array, true);
	pthread_mutex_unlock(&mutex);

	if (success) {
		os_atomic_set_bool(&ingests_refreshed, true);
		os_atomic_set_bool(&ingests_loaded, true);

                onlyfans_ingests_lock();
                // Sorting the list of ingests by RTT.
                bubble_sort_by_rtt(cur_ingests.array, onlyfans_ingest_count());
                onlyfans_ingests_unlock();
	}

	UNUSED_PARAMETER(param);
	return true;
}

static void* onupdate() {
        while (!os_atomic_load_bool(&update_stopped)) {
		const size_t ingests_count = onlyfans_ingest_count();
		for (size_t i = 0; i < ingests_count; ++i) {
			onlyfans_ingests_lock();
                        // Getting ingest.
                        ingest_t * ingest = cur_ingests.array + i;
                        onlyfans_ingests_unlock();
			// Updating ingest RTT.
			ingest->rtt = connection_time(ingest->url, default_port);
		}
                onlyfans_ingests_lock();
		// Sorting the list of ingests by RTT.
                bubble_sort_by_rtt(cur_ingests.array, ingests_count);
                onlyfans_ingests_unlock();

		for (int timeout = update_timeout;
		     timeout > 0 && !os_atomic_load_bool(&update_stopped);
		     --timeout) {
                        os_sleep_ms(1000);
		}
	}
	return EXIT_SUCCESS;
}

void onlyfans_ingests_lock(void) {
	pthread_mutex_lock(&mutex);
}

void onlyfans_ingests_unlock(void) {
	pthread_mutex_unlock(&mutex);
}

size_t onlyfans_ingest_count(void) {
	return cur_ingests.num;
}

struct onlyfans_ingest get_onlyfans_ingest(size_t idx) {
	onlyfans_ingest_t ingest;

	if (cur_ingests.num <= idx) {
		ingest.name = NULL;
		ingest.url = NULL;
	} else {
		ingest = *(onlyfans_ingest_t *)(cur_ingests.array + idx);
	}
	return ingest;
}

void init_onlyfans_data(void) {
	da_init(cur_ingests);
	pthread_mutex_init(&mutex, NULL);
	// Creating a new update thread.
	pthread_create(&thread, NULL, onupdate, NULL);
}

void onlyfans_ingests_refresh(int seconds) {
	if (os_atomic_load_bool(&ingests_refreshed)) {
		return;
	}
	if (!os_atomic_load_bool(&ingests_refreshing)) {
		os_atomic_set_bool(&ingests_refreshing, true);

                onlyfans_update_info = update_info_create_single(
			"[onlyfans ingest update] ",
			get_module_name(),
                        destination_url,
                        onlyfans_ingest_update,
			NULL);
	}

	/* wait five seconds max when loading ingests for the first time */
	if (!os_atomic_load_bool(&ingests_loaded)) {
		for (int i = 0; i < seconds * 100; i++) {
			if (os_atomic_load_bool(&ingests_refreshed)) {
				break;
			}
			os_sleep_ms(10);
		}
	}
}

void load_onlyfans_data(void) {
	char *onlyfans_cache = obs_module_config_path(default_ingests_file_name);
	struct ingest def = {
		.name = bstrdup("Default"),
		.url = bstrdup(default_server),
		.rtt = 0
	};
	pthread_mutex_lock(&mutex);
	da_push_back(cur_ingests, &def);
	pthread_mutex_unlock(&mutex);

	if (os_file_exists(onlyfans_cache)) {
		char *data = os_quick_read_utf8_file(onlyfans_cache);
		bool success = false;

		pthread_mutex_lock(&mutex);
		success = load_ingests(data, false);
		pthread_mutex_unlock(&mutex);

		if (success) {
			os_atomic_set_bool(&ingests_loaded, true);
		}

		bfree(data);
	} else {
                onlyfans_ingests_refresh(3);
	}
	bfree(onlyfans_cache);
}

void unload_onlyfans_data(void) {
        os_atomic_set_bool(&update_stopped, true);
        pthread_join(thread, NULL);
        // Saving OnlyFans cache.
        save_cache_ingests();
	update_info_destroy(onlyfans_update_info);
	free_ingests();
	pthread_mutex_destroy(&mutex);
}
