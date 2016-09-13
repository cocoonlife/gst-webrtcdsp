plugin_LTLIBRARIES = libgstwebrtcdsp.la

# sources used to compile this plug-in
libgstwebrtcdsp_la_SOURCES = gstwebrtcdsp.cpp gstwebrtcechoprobe.cpp \
				gstwebrtcdsp.h gstwebrtcechoprobe.h

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstwebrtcdsp_la_CXXFLAGS = $(GST_CFLAGS) $(WEBRTCDSP_CFLAGS)
libgstwebrtcdsp_la_LIBADD = $(GST_LIBS) $(WEBRTCDSP_LIBS)
libgstwebrtcdsp_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstwebrtcdsp_la_LIBTOOLFLAGS = $(GST_PLUGIN_LIBTOOLFLAGS)

