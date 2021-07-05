MODULE = mod_fillin
C_SRC = $(MODULE).cpp
FILES = $(C_SRC)
OBJECTS = $(FILES:.cpp=.lo)

DEFINES = -DLINUX -D_REENTRANT -D_GNU_SOURCE $(DEBUG)

# Create Makefile.lcl, which should define
# DEST, the folder where the apache module should reside
# INCLUDES, includes for httpd, apr headers

MAKEOPT ?= Makefile.lcl
include $(MAKEOPT)
TARGET = .libs/$(MODULE).so

# Can't use apxs to build c++ modules
# The options used here might depend on how apache was built
$(TARGET)       :       $(OBJECTS)
	$(LIBTOOL) --mode=link g++ -o $(MODULE).la -rpath $(LIBEXECDIR) -module -avoid-version $^ $(LIBS)

%.lo	:	%.cpp
	$(LIBTOOL) --mode=compile g++ -std=c++0x -prefer-pic -O2 -Wall $(DEFINES) $(EXTRA_INCLUDES) -I $(EXP_INCLUDEDIR) -pthread -c -o $@ $< && touch $(@:.lo=.slo)

install : $(TARGET)
	$(SUDO) $(CP) $(TARGET) $(DEST)

clean   :
	$(RM) -r .libs *.{o,lo,slo,la}
