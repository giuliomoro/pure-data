#!/bin/bash
PKGNAME="libpd-xenomai-3-dev"
PROVIDES="libpd-dev"
CONFLICTS=

DIRTY_HASH=`git diff --quiet && git diff --cached --quiet || echo "-dirty"`
TAG=`git describe --tags`
if [ "$TAG" = "`git describe --tags --abbrev=0`" ]
then
	echo "We are on a tag: $TAG $DIRTY_HASH"
fi
VERSION=""
#ensure VERSION starts with a number, or checkinstall will complain
VERSION="`git describe --tags | sed \"s/^[^0-9]*//\"`$DIRTY_HASH"
COMMIT=`git rev-parse HEAD`
BRANCH=`git rev-parse --abbrev-ref HEAD`
REMOTE=`git config --get remote.$BRANCH.url` 

checkinstall --deldoc=yes --backup=no --pkgname="$PKGNAME" --pkgsource="$REMOTE $COMMIT $DIRTY_HASH" --provides="$PROVIDES" --conflicts="$CONFLICTS" --maintainer="`git config --get user.name` \<`git config --get user.email`\>" --pkgversion="$VERSION" -y make -f Makefile-Bela install

