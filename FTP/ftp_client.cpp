/*
 * ftp_client.cpp - Implementação básica de cliente FTP
 *
 * Objetivo: Aprendizagem dos protocolos de rede
 *
 * Compilar: g++ -std=c++17 -Wall ftp_client.cpp -o ftp_client
 * Executar: ./ftp_client <servidor> [porta]
 *
 *
 * Funcionalidades implementadas:
    Conexão básica com servidor FTP
    Autenticação (USER/PASS)
    Modo passivo (PASV)
    Listagem de arquivos (LIST)
    Download de arquivos (RETR)
    Upload de arquivos (STOR)
    Interface interativa de linha de comando
Conceitos de FTP aprendidos:
    Conexão duplex: Socket de controle (comandos) + socket de dados (transferência)
    Modo passivo: Cliente se conecta ao servidor para transferência de dados
    Códigos de resposta FTP: 220, 331, 230, 227, 150, etc.
    Comandos FTP básicos: USER, PASS, PASV, LIST, RETR, STOR, QUIT
    Formato de dados PASV: Parse do formato (h1,h2,h3,h4,p1,p2)
 * Comandos suportados:
 *   USER <username> - Autenticação
 *   PASS <password> - Autenticação
 *   LIST            - Listar arquivos
 *   RETR <arquivo>  - Download
 *   STOR <arquivo>  - Upload
 *   QUIT            - Sair
 */

#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <fstream>
#include <memory>

class FTPClient {
private:
    int control_socket;
    int data_socket;
    std::string server;
    int port;
    bool passive_mode;

public:
    FTPClient() : control_socket(-1), data_socket(-1), port(21), passive_mode(false) {}

    ~FTPClient() {
        disconnect();
    }

    bool connect(const std::string& server, int port = 21) {
        this->server = server;
        this->port = port;

        // Criar socket de controle
        control_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (control_socket < 0) {
            std::cerr << "Erro ao criar socket" << std::endl;
            return false;
        }

        // Resolver nome do servidor
        struct hostent* host = gethostbyname(server.c_str());
        if (!host) {
            std::cerr << "Erro ao resolver servidor: " << server << std::endl;
            return false;
        }

        // Configurar endereço do servidor
        struct sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        memcpy(&server_addr.sin_addr, host->h_addr, host->h_length);

        // Conectar
        if (::connect(control_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            std::cerr << "Erro ao conectar com " << server << ":" << port << std::endl;
            return false;
        }

        // Ler resposta inicial do servidor
        std::string response = read_response();
        std::cout << "Conectado: " << response;

        if (response.substr(0, 3) != "220") {
            std::cerr << "Resposta inesperada do servidor" << std::endl;
            return false;
        }

        return true;
    }

    void disconnect() {
        if (control_socket >= 0) {
            send_command("QUIT");
            close(control_socket);
            control_socket = -1;
        }
        if (data_socket >= 0) {
            close(data_socket);
            data_socket = -1;
        }
    }

    bool login(const std::string& username, const std::string& password) {
        // Enviar USER
        std::string response = send_command("USER " + username);
        if (response.substr(0, 3) != "331") {
            std::cerr << "Erro no usuário: " << response << std::endl;
            return false;
        }

        // Enviar PASS
        response = send_command("PASS " + password);
        if (response.substr(0, 3) != "230") {
            std::cerr << "Erro na senha: " << response << std::endl;
            return false;
        }

        std::cout << "Login realizado com sucesso!" << std::endl;
        return true;
    }

    bool set_passive_mode() {
        std::string response = send_command("PASV");
        if (response.substr(0, 3) != "227") {
            std::cerr << "Erro ao ativar modo passivo: " << response << std::endl;
            return false;
        }

        // Parse da resposta PASV para obter IP e porta
        size_t start = response.find('(');
        size_t end = response.find(')');
        if (start == std::string::npos || end == std::string::npos) {
            std::cerr << "Resposta PASV malformada" << std::endl;
            return false;
        }

        std::string pasv_data = response.substr(start + 1, end - start - 1);
        std::vector<int> numbers;
        std::stringstream ss(pasv_data);
        std::string item;

        while (std::getline(ss, item, ',')) {
            numbers.push_back(std::stoi(item));
        }

        if (numbers.size() != 6) {
            std::cerr << "Dados PASV inválidos" << std::endl;
            return false;
        }

        // Construir IP e porta
        std::string data_ip = std::to_string(numbers[0]) + "." +
                             std::to_string(numbers[1]) + "." +
                             std::to_string(numbers[2]) + "." +
                             std::to_string(numbers[3]);
        int data_port = (numbers[4] << 8) + numbers[5];

        // Conectar socket de dados
        data_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (data_socket < 0) {
            std::cerr << "Erro ao criar socket de dados" << std::endl;
            return false;
        }

        struct sockaddr_in data_addr{};
        data_addr.sin_family = AF_INET;
        data_addr.sin_port = htons(data_port);
        inet_pton(AF_INET, data_ip.c_str(), &data_addr.sin_addr);

        if (::connect(data_socket, (struct sockaddr*)&data_addr, sizeof(data_addr)) < 0) {
            std::cerr << "Erro ao conectar socket de dados" << std::endl;
            close(data_socket);
            data_socket = -1;
            return false;
        }

        passive_mode = true;
        return true;
    }

    std::string list_files() {
        if (!set_passive_mode()) {
            return "";
        }

        std::string response = send_command("LIST");
        if (response.substr(0, 3) != "150") {
            std::cerr << "Erro no LIST: " << response << std::endl;
            return "";
        }

        // Ler dados do socket de dados
        std::string file_list = read_data();
        close(data_socket);
        data_socket = -1;

        // Ler resposta final
        read_response();

        return file_list;
    }

    bool download_file(const std::string& remote_file, const std::string& local_file) {
        if (!set_passive_mode()) {
            return false;
        }

        std::string response = send_command("RETR " + remote_file);
        if (response.substr(0, 3) != "150") {
            std::cerr << "Erro no RETR: " << response << std::endl;
            return false;
        }

        // Receber dados do arquivo
        std::string file_data = read_data();
        close(data_socket);
        data_socket = -1;

        // Salvar arquivo localmente
        std::ofstream file(local_file, std::ios::binary);
        if (!file) {
            std::cerr << "Erro ao criar arquivo local: " << local_file << std::endl;
            return false;
        }

        file.write(file_data.c_str(), file_data.size());
        file.close();

        // Ler resposta final
        read_response();

        std::cout << "Download concluído: " << remote_file << " -> " << local_file << std::endl;
        return true;
    }

    bool upload_file(const std::string& local_file, const std::string& remote_file) {
        if (!set_passive_mode()) {
            return false;
        }

        // Ler arquivo local
        std::ifstream file(local_file, std::ios::binary);
        if (!file) {
            std::cerr << "Erro ao abrir arquivo local: " << local_file << std::endl;
            return false;
        }

        std::string file_data((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
        file.close();

        std::string response = send_command("STOR " + remote_file);
        if (response.substr(0, 3) != "150") {
            std::cerr << "Erro no STOR: " << response << std::endl;
            return false;
        }

        // Enviar dados
        send(data_socket, file_data.c_str(), file_data.size(), 0);
        close(data_socket);
        data_socket = -1;

        // Ler resposta final
        read_response();

        std::cout << "Upload concluído: " << local_file << " -> " << remote_file << std::endl;
        return true;
    }

private:
    std::string read_response() {
        char buffer[4096];
        std::string response;

        while (true) {
            ssize_t bytes_read = recv(control_socket, buffer, sizeof(buffer) - 1, 0);
            if (bytes_read <= 0) {
                break;
            }

            buffer[bytes_read] = '\0';
            response += buffer;

            // Resposta FTP termina com código de 3 dígitos + espaço
            if (response.length() >= 4 && response[3] == ' ') {
                break;
            }
        }

        return response;
    }

    std::string read_data() {
        char buffer[4096];
        std::string data;

        while (true) {
            ssize_t bytes_read = recv(data_socket, buffer, sizeof(buffer) - 1, 0);
            if (bytes_read <= 0) {
                break;
            }

            buffer[bytes_read] = '\0';
            data += buffer;
        }

        return data;
    }

    std::string send_command(const std::string& command) {
        std::string full_command = command + "\r\n";
        send(control_socket, full_command.c_str(), full_command.length(), 0);
        return read_response();
    }
};

// ajuda
void print_usage() {
    std::cout << "Uso: ftp_client <servidor> [porta]" << std::endl;
    std::cout << "Comandos disponíveis:" << std::endl;
    std::cout << "  user <username> - Definir usuário" << std::endl;
    std::cout << "  pass <password> - Definir senha" << std::endl;
    std::cout << "  list            - Listar arquivos" << std::endl;
    std::cout << "  get <arquivo>   - Download" << std::endl;
    std::cout << "  put <arquivo>   - Upload" << std::endl;
    std::cout << "  quit            - Sair" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string server = argv[1];
    int port = (argc > 2) ? std::stoi(argv[2]) : 21;

    FTPClient client;

    if (!client.connect(server, port)) {
        return 1;
    }

    std::cout << "\nCliente FTP conectado. Digite 'help' para ver comandos." << std::endl;

    // fluxo em loop
    std::string line;
    while (true) {
        std::cout << "ftp> ";
        std::getline(std::cin, line);

        if (line.empty()) continue;

        std::stringstream ss(line);
        std::string command;
        ss >> command;

        if (command == "quit" || command == "exit") {
            break;
        }
        else if (command == "help") {
            print_usage();
        }
        else if (command == "user") {
            std::string username;
            ss >> username;
            std::string password;
            std::cout << "Password: ";
            std::getline(std::cin, password);
            client.login(username, password);
        }
        else if (command == "pass") {
            std::string password;
            ss >> password;
            std::cout << "Use 'user' primeiro para especificar username" << std::endl;
        }
        else if (command == "list") {
            std::string files = client.list_files();
            std::cout << files << std::endl;
        }
        else if (command == "get") {
            std::string remote_file, local_file;
            ss >> remote_file;
            if (ss >> local_file) {
                client.download_file(remote_file, local_file);
            } else {
                client.download_file(remote_file, remote_file);
            }
        }
        else if (command == "put") {
            std::string local_file, remote_file;
            ss >> local_file;
            if (ss >> remote_file) {
                client.upload_file(local_file, remote_file);
            } else {
                client.upload_file(local_file, local_file);
            }
        }
        else {
            std::cout << "Comando desconhecido: " << command << std::endl;
            std::cout << "Use 'help' para ver comandos disponíveis" << std::endl;
        }
    }

    client.disconnect();
    std::cout << "Conexão encerrada." << std::endl;

    return 0;
}
