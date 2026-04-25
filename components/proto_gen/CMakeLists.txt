idf_component_register(
    SRCS "traini.pb.c"
    INCLUDE_DIRS "."
    REQUIRES livekit__nanopb
    # nanopb is publicly required because traini.pb.h #include <pb.h>
)
