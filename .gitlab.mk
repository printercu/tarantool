GITLAB_MAKE:=${MAKE} -f .gitlab.mk
TRAVIS_MAKE:=${MAKE} -f .travis.mk

# #####
# Utils
# #####

# Update submodules.
#
# Note: There is no --force option for `git submodule` on git
# 1.7.1, which is shiped in CentOS 6.
git_submodule_update:
	git submodule update --force --recursive --init 2>/dev/null || \
		git submodule update --recursive --init

# Pass *_no_deps goals to .travis.mk.
test_%_no_deps: git_submodule_update
	${TRAVIS_MAKE} $@

# #######################################################
# Build and push testing docker images to GitLab Registry
# #######################################################

# These images contains tarantool dependencies and testing
# dependencies to run tests in them.
#
# How to run:
#
# make GITLAB_USER=foo -f .gitlab.mk docker_bootstrap
#
# The command will prompt for a password. If two-factor
# authentication is enabled an access token with 'api' scope
# should be entered here instead of a password.
#
# When to run:
#
# When some of deps_* goals in .travis.mk are updated.
#
# Keep in a mind that the resulting image is used to run tests on
# all branches, so avoid removing packages: only add them.

GITLAB_REGISTRY?=registry.gitlab.com
DOCKER_BUILD=docker build --network=host -f - .
DOCKERFILE_BUILD=docker build --network=host

define DEBIAN_STRETCH_DOCKERFILE
FROM packpack/packpack:debian-stretch
COPY .travis.mk .
RUN make -f .travis.mk deps_debian
endef
export DEBIAN_STRETCH_DOCKERFILE

define DEBIAN_BUSTER_DOCKERFILE
FROM packpack/packpack:debian-buster
COPY .travis.mk .
RUN make -f .travis.mk deps_buster_clang_8
endef
export DEBIAN_BUSTER_DOCKERFILE

IMAGE_PREFIX:=${GITLAB_REGISTRY}/tarantool/tarantool/testing
DEBIAN_STRETCH_IMAGE:=${IMAGE_PREFIX}/debian-stretch
DEBIAN_BUSTER_IMAGE:=${IMAGE_PREFIX}/debian-buster

TRAVIS_CI_MD5SUM:=$(firstword $(shell md5sum .travis.mk))

docker_bootstrap:
	# Login.
	docker login -u ${GITLAB_USER} ${GITLAB_REGISTRY}
	# Build images.
	echo "$${DEBIAN_STRETCH_DOCKERFILE}" | ${DOCKER_BUILD} \
		-t ${DEBIAN_STRETCH_IMAGE}:${TRAVIS_CI_MD5SUM} \
		-t ${DEBIAN_STRETCH_IMAGE}:latest
	echo "$${DEBIAN_BUSTER_DOCKERFILE}" | ${DOCKER_BUILD} \
		-t ${DEBIAN_BUSTER_IMAGE}:${TRAVIS_CI_MD5SUM} \
		-t ${DEBIAN_BUSTER_IMAGE}:latest
	# Push images.
	docker push ${DEBIAN_STRETCH_IMAGE}:${TRAVIS_CI_MD5SUM}
	docker push ${DEBIAN_BUSTER_IMAGE}:${TRAVIS_CI_MD5SUM}
	docker push ${DEBIAN_STRETCH_IMAGE}:latest
	docker push ${DEBIAN_BUSTER_IMAGE}:latest

docker_perf_bootstrap:
	docker login -u ${CI_REGISTRY_USER} -p ${CI_REGISTRY_PASSWORD} \
		${CI_REGISTRY}
	# TODO: !!! TEMPORARY !!! use from repository
	cp -rfp /home/aleks.tikhonov/Workspaces/runners images/.
	${DOCKERFILE_BUILD} -t ${IMAGE_PERF} -f images/Dockerfile.ubuntu_perf images
	docker push ${IMAGE_PERF}
	${DOCKERFILE_BUILD} --build-arg image_from=${IMAGE_PERF} -t ${IMAGE_PERF_BUILT} -f images/Dockerfile.ubuntu_perf_build .
	docker push ${IMAGE_PERF_BUILT}

# #####################################################
# Remove temporary performance image from the test host
# #####################################################
docker_perf_tmp_image_remove:
	docker rmi --force ${IMAGE_PERF_BUILT}

# #################################
# Run tests under a virtual machine
# #################################

vms_start:
	VBoxManage controlvm ${VMS_NAME} poweroff || true
	VBoxManage snapshot ${VMS_NAME} restore ${VMS_NAME}
	VBoxManage startvm ${VMS_NAME} --type headless

vms_test_%:
	tar czf - ../tarantool | ssh ${VMS_USER}@127.0.0.1 -p ${VMS_PORT} tar xzf -
	ssh ${VMS_USER}@127.0.0.1 -p ${VMS_PORT} "/bin/bash -c \
		'${EXTRA_ENV} \
		cd tarantool && \
		${GITLAB_MAKE} git_submodule_update && \
		${TRAVIS_MAKE} $(subst vms_,,$@)'"

vms_shutdown:
	VBoxManage controlvm ${VMS_NAME} poweroff

# ########################
# Build RPM / Deb packages
# ########################

package: git_submodule_update
	git clone https://github.com/packpack/packpack.git packpack
	PACKPACK_EXTRA_DOCKER_RUN_PARAMS='--network=host' ./packpack/packpack

# ###################
# Performance testing
# ###################

perf_ycsb:
	/opt/runners/ycsb-benchmarking/run.sh ${RUNS}

perf_nosqlbench:
	/opt/runners/nosqlbench-benchmarking/run.sh

perf_cbench:
	/opt/runners/cbench-benchmarking/run.sh
