idf_build_get_property(project_dir PROJECT_DIR)

set(CMAKE_CXX_STANDARD 17)

idf_component_register(
    SRCS httpd.c wifi.c main.c animation.cpp
    INCLUDE_DIRS .
    EMBED_TXTFILES ${project_dir}/web/wifi.html.gz
)