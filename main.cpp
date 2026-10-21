#include <iostream>
#include <fstream>
#include <string>
#include <array>         // для буфера в exec
#include <memory>        // для unique_ptr в exec
#include <stdexcept>     // для runtime_error
#include <sys/inotify.h> // для работы с inotify
#include <unistd.h>      // для read и close

using namespace std;

// файл, который мониторим (можно передать аргументом: ./main /путь/к/файлу)
string file_path = "/home/carvita/projects/project08/file.txt";

// функция из методички: запускает команду cmd и возвращает ее вывод строкой
std::string exec(string cmd) {
	std::array<char, 128> buffer; // буфер для перехвата вывода команды
	std::string result;           // сюда соберем полный вывод команды
	std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose); // создаем процесс
	if (!pipe) {
		throw std::runtime_error("popen() failed!");
	}
	while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) { // пока команда генерит текст
		result += buffer.data(); // подклеиваем его к result
	}
	return result;
}

// читает содержимое файла целиком
string read_file(string path) {
	ifstream ifs(path);
	string content;
	if (ifs.is_open()) {
		content.assign((istreambuf_iterator<char>(ifs)), (istreambuf_iterator<char>()));
		ifs.close();
	}
	return content;
}

// перезаписывает файл новым содержимым (без ios::app — старое стирается)
void write_file(string path, string str) {
	ofstream ofs(path);
	if (ofs.is_open()) {
		ofs << str;
		ofs.close();
	}
}

// оборачивает текст в одинарные кавычки, чтобы безопасно передать его в команду
string quote(string text) {
	string result = "'";
	for (char c : text) {
		if (c == '\'') result += "'\\''";
		else result += c;
	}
	result += "'";
	return result;
}

int main(int argc, char* argv[]) {
	if (argc > 1) file_path = argv[1];

	// создаем файловый дескриптор для отслеживания
	int inotify_fd = inotify_init();
	if (inotify_fd < 0) {
		cout << "ошибка inotify_init" << endl;
		return 1;
	}

	// если файла нет — создаем его (ios::app не стирает файл, если он уже есть).
	// это важно для systemd: при запуске системы никто не сделает touch за нас
	ofstream create(file_path, ios::app);
	create.close();

	// мониторим КОНКРЕТНЫЙ файл, а не папку.
	// IN_MODIFY — содержимое файла изменилось
	int watch_fd = inotify_add_watch(inotify_fd, file_path.c_str(), IN_MODIFY);
	if (watch_fd < 0) {
		cout << "не удалось отслеживать файл " << file_path << " (он существует?)" << endl;
		return 1;
	}

	cout << "-=-=-=-=-=-" << endl
	     << "ЗАПУСКАЮСЬ" << endl
	     << "-=-=-=-=-=-" << endl;
	cout << "мониторю файл " << file_path << endl;

	// history — всё, что сейчас лежит в файле и уже обработано (накопленные рисунки).
	// при запуске берем то, что в файле уже есть
	string history = read_file(file_path);

	// демон работает бесконечно
	while (true) {
		char buffer[10000];

		// ждем, пока файл не изменится
		int length = read(inotify_fd, buffer, 10000);

		int i = 0;
		while (i < length) {
			struct inotify_event *event = (struct inotify_event *)&buffer[i];

			// имени файла в событии нет, поэтому проверяем по номеру слежения:
			// если event->wd совпал с watch_fd — событие про наш файл
			if ((event->mask & IN_MODIFY) && event->wd == watch_fd) {
				string text = read_file(file_path);

				// выясняем, что сделал пользователь:
				// 1) дописал в конец (echo >> или nano под рисунками) — файл начинается
				//    со старых рисунков, новое — только то, что после них, рисунки копятся;
				// 2) стер всё и написал заново (echo > или nano с нуля) — старых рисунков
				//    в начале нет, новое — всё содержимое, старые рисунки больше не нужны
				string new_text;
				bool replaced; // true — файл перезаписан целиком
				if (text.find(history) == 0) {
					new_text = text.substr(history.size());
					replaced = false;
				} else {
					new_text = text;
					replaced = true;
				}

				// убираем переводы строк в конце
				while (!new_text.empty() && (new_text.back() == '\n' || new_text.back() == '\r')) {
					new_text.pop_back();
				}

				// если нового текста нет (файл пустой — nano стер его перед записью,
				// или добавили только пустые строки) — ничего не делаем
				if (new_text.find_first_not_of(" \n\r\t") != string::npos) {
					// всё, что выводим через cout, systemd сохраняет в журнал (journalctl)
					cout << "файл изменился, новый текст:" << endl;
					cout << new_text << endl;

					// если написали "Умри" — специально падаем с ошибкой,
					// чтобы проверить, что systemd перезапустит демона
					if (new_text.find("Умри") != string::npos) {
						cout << "получил команду Умри, падаю" << endl;
						throw runtime_error("Падаю =ООО");
					}

					// рисуем только новый текст
					string art = exec("figlet " + quote(new_text));

					if (replaced) {
						history = art;   // файл стерли — остается только новый рисунок
					} else {
						history += art;  // дописали — новый рисунок под старыми
					}

					// ВАЖНО: отключаем слежение перед записью,
					// иначе наша же запись вызовет новое событие — и так бесконечно
					inotify_rm_watch(inotify_fd, watch_fd);

					// записываем в файл накопленные рисунки
					write_file(file_path, history);
					// дублируем в журнал то, что дописали в файл
					if (replaced) {
						cout << "файл перезаписан, записал в файл:" << endl;
					} else {
						cout << "дописал в файл:" << endl;
					}
					cout << art << endl;

					// включаем слежение обратно
					watch_fd = inotify_add_watch(inotify_fd, file_path.c_str(), IN_MODIFY);
				}
			}

			// сдвигаемся на следующее событие
			i += sizeof(struct inotify_event) + event->len;
		}
	}

	inotify_rm_watch(inotify_fd, watch_fd);
	close(inotify_fd);
	return 0;
}
