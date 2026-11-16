idf_component_register(
    SRCS
        "img_recv.c"
        "fatfs_store.c"
    INCLUDE_DIRS "include"
    REQUIRES
        display
        fatfs
        wear_levelling
        espressif__cjson
)
