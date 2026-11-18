# The receiver's own firmware: power sequencing and governor, test signals,
# the test API (HTTP + console + mDNS) and the web app. core/ is plain C and
# host-tested (firmware/test); esp/ ties it to ESP-IDF and squeezelite-esp32.
set(srcs
	core/hifi_api.c core/hifi_b64.c core/hifi_boost.c core/hifi_boot.c core/hifi_capture.c core/hifi_diag.c core/hifi_exp.c core/hifi_gate.c
	core/hifi_gov.c core/hifi_ir.c core/hifi_json.c core/hifi_lab.c core/hifi_line.c core/hifi_link.c core/hifi_listen.c core/hifi_meta.c core/hifi_ota.c core/hifi_party.c
	core/hifi_party_group.c core/hifi_party_sync.c core/hifi_pd.c
	core/hifi_power.c core/hifi_quiet.c core/hifi_remote.c core/hifi_scan.c core/hifi_sha256.c core/hifi_source.c core/hifi_tap.c core/hifi_tone.c core/hifi_update.c
	core/hifi_chan.c core/hifi_vol.c core/hifi_ws_proto.c esp/hifi_main.c esp/hifi_crash.c esp/hifi_diag_esp.c esp/hifi_dl.c esp/hifi_http.c esp/hifi_listen_esp.c esp/hifi_net.c esp/hifi_party_esp.c esp/hifi_relay.c esp/hifi_ws.c)

# the board's own code (esp/hifi_bsp.h), from the Kconfig board choice
if(CONFIG_HIFI_PLUS OR CONFIG_HIFI_ULTRA)
	list(APPEND srcs esp/board_core.c)
else()
	list(APPEND srcs esp/board_lite.c)
endif()

# the lab build (board/lite-lab): experiments a signed command starts and stops
if(CONFIG_HIFI_LAB)
	list(APPEND srcs esp/hifi_lab_esp.c esp/lab_espnow.c esp/lab_lanscan.c esp/lab_forge.c esp/lab_wifidrop.c esp/lab_crafted.c)
	# reads the board's I2C chips through hive-fw's i2c-device driver (esp/i2cdev)
	list(APPEND srcs esp/lab_i2c.c esp/i2cdev/i2cdev.c esp/i2cdev/i2cdev_esp.c)
	set(lab_inc esp/i2cdev)
	# Bluetooth off or quiet for a while, to measure what it costs Wi-Fi
	if(CONFIG_BT_CLASSIC_ENABLED)
		# and a real A2DP stream between two boards, beside the other radios
		list(APPEND srcs esp/lab_bt.c esp/lab_a2dp.c)
	endif()
	# the BLE lab image (board/lite-lab-ble): a GATT link beside A2DP
	if(CONFIG_BT_BLE_ENABLED)
		list(APPEND srcs esp/lab_ble.c)
	endif()
	# the lab-radio image (board/lite-lab-radio): 96/24 FLAC decoded on a
	# PSRAM stack beside the radios, from a clip in the image
	if(CONFIG_HIFI_LAB_PSRAM_STACKS)
		list(APPEND srcs esp/lab_flac.c)
		set(lab_embed esp/lab_assets/clip96k24.flac)
	endif()
	# a stand-in Spotify app on the Spotify sink's handlers, to time the
	# handover with Bluetooth (patch 0034 adds the hooks and -u lab_phone)
	if(CONFIG_CSPOT_SINK)
		list(APPEND srcs esp/lab_phone.c)
	endif()
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
	PRIV_INCLUDE_DIRS esp ${webapp_dir} ${lab_inc}
	EMBED_FILES ${lab_embed}
	REQUIRES esp_http_server
	PRIV_REQUIRES driver esp_adc_cal mdns console nvs_flash esp_wifi pthread
		      platform_config services wifi-manager tools
		      app_update mqtt libsodium mbedtls lwip esp_http_client json
		      tcp_transport esp-tls bt codecs)

# The Hive's signing key(s), an optional built-in relay and update manifest,
# from the build's environment (firmware/tools/build.sh; docs/remote.md).
# Never in git.
foreach(v HIFI_HIVE_PUBKEY HIFI_RELAY_URL HIFI_RELAY_USER HIFI_RELAY_PASS HIFI_UPDATE_URL)
	if(DEFINED ENV{${v}})
		target_compile_definitions(${COMPONENT_LIB} PRIVATE ${v}="$ENV{${v}}")
	endif()
endforeach()

# the build number the LAN's updates are held to (hifi_relay.c, security.md L9)
if(DEFINED ENV{HIFI_FW_BUILD})
	target_compile_definitions(${COMPONENT_LIB} PRIVATE HIFI_FW_BUILD=$ENV{HIFI_FW_BUILD})
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wno-format-truncation)

# party mode carries 16-bit frames; squeezelite built 32-bit (DEPTH 32) calls
# none of its output hooks (patches/0021), so party says it can't play there
if(DEFINED DEPTH AND DEPTH EQUAL 32)
	target_compile_definitions(${COMPONENT_LIB} PRIVATE HIFI_PARTY_NO_AUDIO=1)
endif()

# esp/hifi_crash.c keeps a copy of everything the console prints (crash
# reports included) across a restart, by wrapping the UART's FIFO write
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=uart_hal_write_txfifo")
# and notes who called esp_restart, so a clean restart names its cause
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_restart")

# squeezelite reaches the LAN gate's hooks through weak references
# (firmware/patches/0009), which don't pull an object out of an archive by
# themselves: name them here so a link-order change can't silently drop the
# gate and leave port 80 open (security.md L1)
foreach(sym hifi_lan_register hifi_lan_command_ok hifi_lan_config_ok hifi_lan_config_secret
	    hifi_ap_password_pick hifi_ap_password_make hifi_ap_due hifi_http_register
	    hifi_restart_note)
	target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,-u,${sym}")
endforeach()
# party mode's output hooks (patch 0021) come with hifi_party_esp.c, which
# patch 0031 links into the app only: naming them here would put it in recovery
