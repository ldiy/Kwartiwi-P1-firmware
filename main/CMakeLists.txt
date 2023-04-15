idf_component_register(SRCS "main.c" "emucs_p1.c" "networking.c" "web_server.c"
                    INCLUDE_DIRS "." "include")


spiffs_create_partition_image(www ../frontend FLASH_IN_PROJECT)