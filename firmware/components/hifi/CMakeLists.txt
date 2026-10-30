# The receiver's own firmware: power sequencing and governor, test signals,
# the test API (HTTP + console + mDNS) and the web app. core/ is plain C and
# host-tested (firmware/test); esp/ ties it to ESP-IDF and squeezelite-esp32.
set(srcs
	core/hifi_api.c core/hifi_b64.c core/hifi_boot.c core/hifi_capture.c core/hifi_exp.c
	core/hifi_gov.c core/hifi_ir.c core/hifi_json.c core/hifi_meta.c core/hifi_ota.c core/hifi_pd.c
	core/hifi_power.c core/hifi_remote.c core/hifi_sha256.c core/hifi_tone.c core/hifi_update.c
	core/hifi_vol.c esp/hifi_main.c esp/hifi_http.c esp/hifi_net.c esp/hifi_relay.c)

# the board's own code (esp/hifi_bsp.h), from the Kconfig board choice
if(CONFIG_HIFI_PLUS OR CONFIG_HIFI_ULTRA)
	list(APPEND srcs esp/board_core.c)
else()
	list(APPEND srcs esp/board_lite.c)
endif()

# app/build.py writes the web app here (firmware/tools/fetch.sh runs it);
# without it a placeholder page is served.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/esp/webapp/webapp_files.c")
	list(APPEND srcs esp/webapp/webapp_files.c)
	set(webapp_dir esp/webapp)
else()
	list(APPEND srcs esp/webapp_placeholder/webapp_files.c)
	set(webapp_dir esp/webapp_placeholder)
endif()

idf_component_register(
	SRCS ${srcs}
	INCLUDE_DIRS core
	PRIV_INCLUDE_DIRS esp ${webapp_dir}
	REQUIRES esp_http_server
	PRIV_REQUIRES driver esp_adc_cal mdns console nvs_flash esp_wifi
		      platform_config services wifi-manager tools
		      app_update mqtt libsodium mbedtls lwip esp_http_client json)

# The Hive's signing key(s), an optional built-in relay and update manifest,
# from the build's environment (firmware/tools/build.sh; docs/remote.md).
# Never in git.
foreach(v HIFI_HIVE_PUBKEY HIFI_RELAY_URL HIFI_RELAY_USER HIFI_RELAY_PASS HIFI_UPDATE_URL)
	if(DEFINED ENV{${v}})
		target_compile_definitions(${COMPONENT_LIB} PRIVATE ${v}="$ENV{${v}}")
	endif()
endforeach()

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wno-format-truncation)
