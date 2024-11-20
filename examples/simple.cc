#include <iostream>
#include <algorithm>
#include <string>
#include <memory>

#include "ut/lock_free_list.h"

struct User_data {
  explicit User_data(int id = 0, std::string name = "") 
    : m_id(id), m_name(std::move(name)) {}

  ut::Node& get_node() noexcept {
    return m_node;
  }

  int m_id{};
  ut::Node m_node{};
  std::string m_name{};
};

using User_data_list = ut::List<User_data, &User_data::get_node>;

void print_forward(const User_data_list& list) {
  std::cout << "Forward iteration:\n";
  for (const auto& user : list) {
    std::cout << "ID: " << user.m_id << " Name: " << user.m_name << "\n";
  }
  std::cout << std::endl;
}

void print_reverse(const User_data_list& list) {
  std::cout << "Reverse iteration:\n";
  for (auto it = list.rbegin(); it != list.rend(); ++it) {
    std::cout << "ID: " << it->m_id << " Name: " << it->m_name << "\n";
  }
  std::cout << std::endl;
}

void modify_names(User_data_list& list) {
  std::cout << "Modifying names using non-const iterator:\n";
  for (auto& user : list) {
    user.m_name += "_modified";
    std::cout << "ID: " << user.m_id << " Name: " << user.m_name << "\n";
  }
  std::cout << std::endl;
}

void demonstrate_algorithms(const User_data_list& list) {
  std::cout << "Using standard algorithms:\n";
  
  /* Find user with ID 2 */
  auto it = std::find_if(list.begin(), list.end(),
    [](const User_data& user) { return user.m_id == 2; });
    
  if (it != list.end()) {
    std::cout << "Found user: " << it->m_name << "\n";
  }
  
  /* Count users with modified names */
  auto count = std::count_if(list.begin(), list.end(),
    [](const User_data& user) { 
      return user.m_name.find("_modified") != std::string::npos; 
    });
  std::cout << "Modified users count: " << count << "\n";
  
  /* Check if all IDs are positive */
  bool all_positive = std::all_of(list.begin(), list.end(),
    [](const User_data& user) { return user.m_id > 0; });
  std::cout << "All IDs positive: " << std::boolalpha << all_positive << "\n\n";
}

int main() {
  constexpr size_t MAX_USERS = 1000;
  auto data = std::make_unique<User_data[]>(MAX_USERS);
  
  auto list = std::make_unique<User_data_list>(
    data.get(), data.get() + MAX_USERS);
    
  data[0] = User_data(1, "Amritsar");
  data[1] = User_data(2, "Benares");
  data[2] = User_data(3, "Chennai");
  data[3] = User_data(4, "Delhi");
  
  for (int i{}; i < 4; ++i) {
    list->push_back(data[i].m_node);
  }
  
  std::cout << "1. Basic Iterator Usage\n" << std::string(50, '-') << "\n";

  print_forward(*list);
  print_reverse(*list);
  
  std::cout << "2. Modifying Elements\n" << std::string(50, '-') << "\n";

  modify_names(*list);
  
  std::cout << "3. STL Algorithm Integration\n" << std::string(50, '-') << "\n";

  demonstrate_algorithms(*list);
  
  /* Manual iterator manipulation */
  std::cout << "4. Manual Iterator Operations\n" << std::string(50, '-') << "\n";
  
  auto it = list->begin();

  std::cout << "First element: " << it->m_name << "\n";
  
  ++it;

  std::cout << "Second element: " << it->m_name << "\n";
  
  --it;

  std::cout << "Back to first: " << it->m_name << "\n";
  
  it = list->end();

  std::cout << "Distance from begin to end: " << std::distance(list->begin(), it) << "\n";
  
  return 0;
}

