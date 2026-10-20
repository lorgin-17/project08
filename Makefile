build:
	@g++ main.cpp -o main -Wno-ignored-attributes

restart: build
	@sudo systemctl restart my_daemon.service
	@echo "демон пересобран и перезапущен"
