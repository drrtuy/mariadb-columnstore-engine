include_directories(${ENGINE_COMMON_INCLUDES})

add_subdirectory(shared)
add_subdirectory(dictionary)
add_subdirectory(xml)
add_subdirectory(wrapper)
add_subdirectory(bulk)
add_subdirectory(client)
add_subdirectory(redistribute)
add_subdirectory(splitter)
add_subdirectory(server)

# ########## install files ###############

# original Makefile.am contents follow:

# Copyright (C) 2014 InfiniDB, Inc.
#
# This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public
# License as published by the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
# warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with this program; if not, write to the Free
# Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
# $Id: Makefile.am 878 2009-04-03 20:34:32Z rdempsey $
#
# SUBDIRS = shared dictionary xml wrapper bulk client redistribute splitter \ server
#
# test:
#
# coverage:
#
# leakcheck:
#
# docs:
#
# bootstrap: for subdir in $(SUBDIRS); \ do $(MAKE) -C $$subdir bootstrap || exit $$?; \ done
#
