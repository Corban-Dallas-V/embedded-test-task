.PHONY: build run clean up down logs

# Имя образа
IMAGE_NAME = embedded-test

build:
	docker compose build

up:
	docker compose up -d

down:
	docker compose down

logs:
	docker compose logs -f

clean:
	docker compose down -v
	docker rmi $(IMAGE_NAME) 