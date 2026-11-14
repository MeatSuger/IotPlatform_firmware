idf_component_register(
    SRCS
        "src/img_recv.c"
        "src/fatfs_store.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        periph
        fatfs
        wear_levelling
        espressif__cjson
)
