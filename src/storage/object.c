#include "object.h"

const char *objtype_name(ObjType t) {
    switch (t) {
        case OBJ_DB:    return "database";
        case OBJ_TABLE: return "table";
        case OBJ_INDEX: return "index";
    }
    return "?";
}
