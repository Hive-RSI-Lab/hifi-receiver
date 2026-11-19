# hifi (third_party/eq, copied over components/audio by tools/fetch.sh for a
# board with CONFIG_HIFI_PUBLISHABLE_CODECS=y): our own 10-band equalizer in
# place of Espressif's prebuilt lib/libesp_processing.a
idf_component_register(SRCS hifi_eq.c link_helper.c
						INCLUDE_DIRS . inc
						)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
