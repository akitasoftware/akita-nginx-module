
PLATFORMS := ubuntu2204 ubuntu2004 amazon2

base_images: $(addprefix build_, $(PLATFORMS))
push: $(addprefix push_, $(PLATFORMS))

# Cannot list the individual targets here as PHONY targets are ignored
# during implicit rule search.
.PHONY: base_images push

VERSION=0.0.1

build_%: Dockerfile.%
	docker build -t akitasoftware/nginx-build-$* -f Dockerfile.$* .

push_%: build_%
	docker tag akitasoftware/nginx-build-$*:${VERSION} akitasoftware/nginx-build-$*:latest
	docker push akitasoftware/nginx-build-$*:${VERSION}
	docker push akitasoftware/nginx-build-$*:latest

