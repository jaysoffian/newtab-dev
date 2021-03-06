#!/bin/bash

set -x

: ${TOOLTOOL_SERVER:=https://api.pub.build.mozilla.org/tooltool/}
: ${TOOLTOOL_REPO:=https://git.mozilla.org/build/tooltool.git}
: ${TOOLTOOL_REV:=master}
: ${SPIDERMONKEY_VARIANT:=plain}
: ${UPLOAD_DIR:=$HOME/artifacts/}
: ${WORK:=$HOME/workspace}
: ${SRCDIR:=$WORK/build/src}

cd $WORK

# Need to install things from tooltool. Figure out what platform to use.

case $(uname -m) in
    i686 | arm )
        BITS=32
        ;;
    *)
        BITS=64
        ;;
esac

case "$OSTYPE" in
    darwin*)
        PLATFORM_OS=macosx
        ;;
    linux-gnu)
        PLATFORM_OS=linux
        ;;
    msys)
        PLATFORM_OS=win
        ;;
    *)
        echo "Unrecognized OSTYPE '$OSTYPE'" >&2
        PLATFORM_OS=linux
        ;;
esac

# Install everything needed for the browser on this platform. Not all of it is
# necessary for the JS shell, but it's less duplication to share tooltool
# manifests.
BROWSER_PLATFORM=$PLATFORM_OS$BITS
TOOLTOOL_MANIFEST="$SRCDIR/browser/config/tooltool-manifests/$BROWSER_PLATFORM/releng.manifest"

tc-vcs checkout $WORK/tooltool $TOOLTOOL_REPO $TOOLTOOL_REPO $TOOLTOOL_REV
(cd $WORK && python tooltool/tooltool.py --url $TOOLTOOL_SERVER -m $TOOLTOOL_MANIFEST fetch ${TOOLTOOL_CACHE:+ -c $TOOLTOOL_CACHE})

# Point to the appropriate compiler, assuming taskcluster will one day run on non-Linux OSes.
case "$PLATFORM_OS" in
    linux)
        export PATH="$WORK/gcc/bin":$PATH
        export GCCDIR="$WORK/gcc"
        ;;
    macosx)
        export PATH="$WORK/clang/bin":$PATH
        ;;
esac

# Run the script
AUTOMATION=1 $SRCDIR/js/src/devtools/automation/autospider.sh $SPIDERMONKEY_VARIANT
BUILD_STATUS=$?

# Ensure upload dir exists
mkdir -p $UPLOAD_DIR

# Copy artifacts for upload by TaskCluster
cp -rL $SRCDIR/obj-spider/dist/bin/{js,jsapi-tests,js-gdb.py} $UPLOAD_DIR

exit $BUILD_STATUS
