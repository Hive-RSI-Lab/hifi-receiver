# The receiver's own firmware: power sequencing and governor, test signals,
# the test API (HTTP + console + mDNS) and the web app. core/ is plain C and
# host-tested (firmware/test); esp/ ties it to ESP-IDF and squeezelite-esp32.
set(srcs
	core/hifi_api.c core/hifi_gov.c core/hifi_json.c core/hifi_pd.c
	core/hifi_ir.c core/hifi_meta.c core/hifi_power.c core/hifi_tone.c core/hifi_vol.c
	esp/hifi_main.c esp/hifi_http.c)

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
		      platform_config services wifi-manager tools)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wno-format-truncation)
