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
string exec(string cmd) {
	array<char, 128> buffer; // буфер для перехвата вывода команды
	string result;           // сюда соберем полный вывод команды
	unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose); // создаем процесс
	if (!pipe) {
		throw runtime_error("popen() failed!");
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

	cout << "мониторю файл " << file_path << endl;

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

				// пустой файл не обрабатываем.
				// это важно для IN_MODIFY: редактор (например nano) при сохранении
				// сначала стирает файл — приходит событие, а файл пустой — пропускаем,
				// потом записывает текст — приходит второе событие, его и обрабатываем
				if (text != "") {
					// всё, что выводим через cout, systemd сохраняет в журнал (journalctl)
					cout << "файл изменился, новое содержимое:" << endl;
					cout << text << endl;

					// прогоняем содержимое файла через figlet
					string art = exec("figlet < \"" + file_path + "\"");

					// ВАЖНО: отключаем слежение перед записью,
					// иначе наша же запись вызовет новое событие,
					// программа снова прогонит файл через figlet — и так бесконечно
					inotify_rm_watch(inotify_fd, watch_fd);

					write_file(file_path, art);
					// дублируем в журнал то, что записали в файл
					cout << "записал в файл:" << endl;
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
