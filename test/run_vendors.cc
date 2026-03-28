#include <string>
#include <iostream>
#include <fstream>
#include <thread>
#include <vector>
#include <memory>
#include <unordered_map>
#include <cstdlib>

extern void run_server(const std::string, const std::string);

void run_vendors(const std::unordered_map<std::string, std::vector<std::string>>& backends);

std::string trim(const std::string& value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

int main(int argc, char** argv) {

  std::string filename;
  if (argc == 2) {
    filename = std::string(argv[1]);
  }
  else {
    std::cerr << "Correct usage: ./run_vendors $file_path_for_server_address" << std::endl;
    return EXIT_FAILURE;
  }

  std::unordered_map<std::string, std::vector<std::string>> backends;
  
  std::ifstream myfile (filename);
  if (myfile.is_open()) {
    std::string line;
    std::string current_vendor;
    while (getline(myfile, line)) {
      line = trim(line);
      if (line.empty()) {
        continue;
      }
      if (line[0] != '-') {
        current_vendor = line;
        backends[current_vendor];
        continue;
      }
      if (current_vendor.empty()) {
        std::cerr << "Malformed vendor file. Backend appears before vendor name." << std::endl;
        return EXIT_FAILURE;
      }
      const std::string backend_address = trim(line.substr(1));
      if (backend_address.empty()) {
        std::cerr << "Malformed vendor file. Empty backend address." << std::endl;
        return EXIT_FAILURE;
      }
      backends[current_vendor].push_back(backend_address);
    }
    myfile.close();
  }
  else {
    std::cerr << "Failed to open file " << filename << std::endl;
    return EXIT_FAILURE;
  }

  for (const auto& vendor_backends : backends) {
    if (vendor_backends.second.empty()) {
      std::cerr << "Vendor '" << vendor_backends.first << "' has no backend addresses." << std::endl;
      return EXIT_FAILURE;
    }
  }

  run_vendors(backends);
  return EXIT_SUCCESS;
}


void run_vendors(const std::unordered_map<std::string, std::vector<std::string>>& backends) {

  typedef std::unique_ptr<std::thread> ThreadPtr;
  std::vector<ThreadPtr> threads;

  for (const auto& vendor_backends : backends) {
    for (const auto& backend_address : vendor_backends.second) {
      threads.push_back(ThreadPtr(new std::thread(run_server, vendor_backends.first, backend_address)));
    }
  }

  for (auto& thread : threads) {
    thread->join();
  }
}
