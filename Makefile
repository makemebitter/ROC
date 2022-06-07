# Copyright 2018 Stanford University
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

ifndef LG_RT_DIR
#$(error LG_RT_DIR variable is not defined, aborting build)
LG_RT_DIR	?= legion/runtime
endif

# Flags for directing the runtime makefile what to include
DEBUG           ?= 1		# Include debugging symbols
OUTPUT_LEVEL    ?= LEVEL_DEBUG	# Compile time logging level
SHARED_LOWLEVEL ?= 0		# Use shared-memory runtime (not recommended)
USE_CUDA        ?= 1		# Include CUDA support (requires CUDA)
USE_GASNET      ?= 0		# Include GASNet support (requires GASNet)
USE_HDF         ?= 0		# Include HDF5 support (requires HDF5)
ALT_MAPPERS     ?= 0		# Include alternative mappers (not recommended)

# Put the binary file name here
OUTFILE		?= gnn
# List all the application source files here
GEN_SRC		?= gnn.cc gnn_mapper.cc scattergather.cc graphnorm.cc linear.cc activation.cc element.cc\
		resourcemanager.cc optimizer.cc	initializer.cc dropout.cc softmax.cc # .cc files
GEN_GPU_SRC	?= cuda_helper.cu load_task.cu nccl_task.cu scattergather_kernel.cu graphnorm_kernel.cu \
		activation_kernel.cu element_kernel.cu linear_kernel.cu optimizer_kernel.cu \
		types.cu dropout_kernel.cu initializer_kernel.cu softmax_kernel.cu # .cu files

# You can modify these variables, some will be appended to by the runtime makefile
INC_FLAGS	?= -Inccl/build/include 
CC_FLAGS	?=
NVCC_FLAGS	?=
GASNET_FLAGS	?=
LD_FLAGS	?= -lcudart -lcuda -lcudnn -lcublas -lcurand -lnccl -Lnccl/build/lib
# For Point and Rect typedefs
CC_FLAGS	+= -std=c++11
NVCC_FLAGS	+= -std=c++11 

###########################################################################
#
#   Don't change anything below here
#   
###########################################################################

include $(LG_RT_DIR)/runtime.mk

