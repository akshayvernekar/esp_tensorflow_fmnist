idf_component_register(SRCS app_main.c app_wifi.c app_httpd.c app_camera.c app_tensorflow.cc model_settings.c image_util/image_util.c tf_model/model_data_28_28.cc
                    INCLUDE_DIRS . image_util/include lib/include
                    EMBED_FILES "www/index.html.gz")

target_link_libraries(${COMPONENT_TARGET} "-L ${CMAKE_CURRENT_SOURCE_DIR}/lib")