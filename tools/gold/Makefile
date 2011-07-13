#===- tools/gold/Makefile ----------------------------------*- Makefile -*-===##
# 
#                     The LLVM Compiler Infrastructure
#
# This file is distributed under the University of Illinois Open Source
# License. See LICENSE.TXT for details.
# 
##===----------------------------------------------------------------------===##

LEVEL = ../..
LIBRARYNAME = LLVMgold

EXPORTED_SYMBOL_FILE = $(PROJ_SRC_DIR)/gold.exports

# Include this here so we can get the configuration of the targets
# that have been configured for construction. We have to do this 
# early so we can set up LINK_COMPONENTS before including Makefile.rules
include $(LEVEL)/Makefile.config

LINK_LIBS_IN_SHARED=1
SHARED_LIBRARY = 1
LOADABLE_MODULE = 1

LINK_COMPONENTS := support

#################################################
# @LOCALMOD-BEGIN
#
# This bugfix has been upstreamed as of LLVM revision 132352.
# All local changes to this file can be removed after merging.

CXXFLAGS+=-I$(BINUTILS_INCDIR) -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64
CXXFLAGS+=$(SharedLibDir)/$(SharedPrefix)LTO$(SHLIBEXT)

# @LOCALMOD-END
#################################################

include $(LEVEL)/Makefile.common
