COMPOSEFS_HASH_CFLAGS = -DUSE_OBSTACK=0 -DTESTING=0 -DUSE_DIFF_HASH=0

libcomposefs_la_SOURCES = \
                        composefs/libcomposefs/bitrotate.h \
                        composefs/libcomposefs/erofs_fs.h \
                        composefs/libcomposefs/erofs_fs_wrapper.h \
                        composefs/libcomposefs/hash.c \
                        composefs/libcomposefs/hash.h \
                        composefs/libcomposefs/lcfs-internal.h \
                        composefs/libcomposefs/lcfs-erofs.h \
                        composefs/libcomposefs/lcfs-fsverity.c \
                        composefs/libcomposefs/lcfs-fsverity.h \
                        composefs/libcomposefs/lcfs-writer-erofs.c \
                        composefs/libcomposefs/lcfs-writer.c \
                        composefs/libcomposefs/lcfs-writer.h \
                        composefs/libcomposefs/lcfs-utils.h \
                        composefs/libcomposefs/lcfs-mount.c \
                        composefs/libcomposefs/lcfs-mount.h \
                        composefs/libcomposefs/xalloc-oversized.h
libcomposefs_la_CFLAGS = $(WARN_CFLAGS) $(COMPOSEFS_HASH_CFLAGS) $(LCFS_DEP_CRYPTO_CFLAGS) $(HIDDEN_VISIBILITY_CFLAGS)
libcomposefs_la_LIBADD = $(LCFS_DEP_CRYPTO_LIBS)
