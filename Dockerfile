FROM ubuntu:18.04

# This docker image is for building and testing mjpegtools.
LABEL version="1.0" maintainer="Attila Bognar <attila.bognar@logmein.com>" product_name="LiveGuide" product_team_name="LiveGuide Team" lmi_org_name="CES"

RUN apt-get update \
    && apt-get install -y build-essential g++-mingw-w64 libpng-dev libdv-dev libjpeg-dev