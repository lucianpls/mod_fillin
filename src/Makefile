MODULE = mod_ahtse_fill
C_SRC = $(MODULE).cpp
FILES = $(C_SRC)
OBJECTS = $(FILES:.cpp=.lo)

DEFINES = -DLINUX -D_REENTRANT -D_GNU_SOURCE $(DEBUG)

# Create Makefile.lcl, which should define
# DEST, the folder where the apache module should reside
# INCLUDES, includes for httpd, apr headers

include Makefile.lcl
TARGET = .libs/$(MODULE).so

# Can't use apxs to build c++ modules
# The options used here might depend on how apache was built
$(TARGET)       :       $(OBJECTS)
	$(LIBTOOL) --mode=link g++ -o $(MODULE).la -rpath $(DEST) -module -avoid-version $^ $(LIBS)

%.lo	:	%.cpp
	$(LIBTOOL) --mode=compile g++ -std=c++0x -prefer-pic -O2 -Wall $(DEFINES) $(INCLUDES) -pthread -c -o $@ $< && touch $(@:.lo=.slo)

install : $(TARGET)
	$(SUDO) cp $(TARGET) $(DEST)

clean   :
	rm -rf .libs *.{o,lo,slo,la}
