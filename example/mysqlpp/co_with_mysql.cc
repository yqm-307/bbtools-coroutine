#include <bbt/coroutine/coroutine.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <iomanip>

#include "user_table.hpp"

void example1()
{
    bbt::core::thread::CountDownLatch latch{1};

    bbtco [&]()
    {
        try {
            // 创建数据库连接
            // MySQLConnection conn{"test_db", "127.0.0.1", "root", "200101"};
            MySQLConnection conn{"127.0.0.1", "root", "200101", "test_db", 3306};
            
            ResultSet resultSet = conn.ExecuteQuery("SELECT * FROM log_data LIMIT 10");
            
            MySQLResultParser::PrintResultSet(resultSet);

            latch.Down();
        } catch (const std::exception& e) {
            std::cerr << "Database exception: " << e.what() << std::endl;
            latch.Down();
        }
    };

    latch.Wait();
}



void example2(std::shared_ptr<UserService> userService)
{
    std::cout << "开始高级MySQL连接池示例..." << std::endl;
    
    bbt::core::thread::CountDownLatch mainLatch{1};
    
    bbtco [userService, &mainLatch]() {
        try {
            // 1. 创建用户表
            if (!userService->CreateUserTable()) {
                std::cerr << "创建用户表失败" << std::endl;
                mainLatch.Down();
                return;
            }
            
            // 2. 插入一些测试用户
            std::cout << "\n插入测试用户..." << std::endl;
            std::vector<User> testUsers = {
                {0, "Alice", "alice@example.com", 25},
                {0, "Bob", "bob@example.com", 30},
                {0, "Charlie", "charlie@example.com", 35}
            };
            
            for (const auto& user : testUsers) {
                userService->InsertUser(user);
                // bbtco_sleep(10); // 稍微延迟
                bbtco_yield;
            }
            
            // 3. 查询所有用户
            std::cout << "\n查询所有用户..." << std::endl;
            auto users = userService->GetAllUsers();
            for (const auto& user : users) {
                std::cout << "ID: " << user.id << ", Name: " << user.name 
                         << ", Email: " << user.email << ", Age: " << user.age << std::endl;
            }
            
            // 4. 更新用户
            if (!users.empty()) {
                std::cout << "\n更新第一个用户..." << std::endl;
                User updateUser = users[0];
                updateUser.name = "Updated " + updateUser.name;
                updateUser.age += 1;
                userService->UpdateUser(updateUser);
            }
            
            // 5. 再次查询验证更新
            std::cout << "\n验证更新结果..." << std::endl;
            users = userService->GetAllUsers();
            for (const auto& user : users) {
                std::cout << "ID: " << user.id << ", Name: " << user.name 
                         << ", Email: " << user.email << ", Age: " << user.age << std::endl;
            }
            
        } catch (const std::exception& e) {
            std::cerr << "主流程错误: " << e.what() << std::endl;
        }
        
        mainLatch.Down();
    };
    
    mainLatch.Wait();
}

void example3(std::shared_ptr<UserService> userService)
{
    std::cout << "\n=== 模拟高并发用户操作 ===" << std::endl;
    
    const int total_users = 2000;
    const int update_co = 10;
    const int update_users = 1000;
    auto cond = bbtco_make_cocond();
    std::atomic<int> process_done_count{0};
    std::atomic<int> update_success_count{0};
    std::atomic<int> update_error_count{0};
    
    bbtco [&](){
        try {

            if (userService->CreateUserTable() == false) {
                std::cerr << "创建用户表失败" << std::endl;
                return;
            }
            
            userService->ClearTable();
            
            for (int i = 0; i < total_users; ++i) {
                User user;
                user.name = "User" + std::to_string(i);
                user.email = "user" + std::to_string(i) + "@example.com";
                user.age = 20 + i % 59;
                
                if (!userService->InsertUser(user)) {
                    std::cerr << "插入测试用户失败: " << user.name << std::endl;
                }
            }
            cond->NotifyAll();
        } catch (const std::exception& e) {
            std::cerr << "初始化用户服务失败: " << e.what() << std::endl;
            cond->NotifyAll();
            return;
        }
    };

    cond->Wait();

    auto start = std::chrono::steady_clock::now();

    // 开启10个协程不停的update用户
    for (int i = 0; i < update_co; ++i)
    {
        bbtco_desc("update user") [userService, &process_done_count, &update_success_count, &update_error_count, i]() {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(1, 100);

            for (int i = 0; i < update_users; ++i)
            {
                User user;
                user.id = (i % 3) + 1; // 更新ID 1-3的用户
                user.name = "UpdatedUser" + std::to_string(i);
                user.email = "updated" + std::to_string(i) + "@example.com";
                user.age = dis(gen);
                std::cout << "co=" << bbt::co::GetLocalCoroutineId() << "\tcount=" << i << "\t更新用户: " << user.name << std::endl;

                if (userService->UpdateUser(user)) {
                    update_success_count++;
                } else {
                    update_error_count++;
                    std::cout << "co=" << bbt::co::GetLocalCoroutineId() << "\t更新用户失败: " << user.name << std::endl;
                }

            }

            process_done_count++;
        };
    }

    while (process_done_count < update_co)
        bbtco_sleep(100);
    
    bbtco_sleep(1000); // 等待所有协程完成
    std::cout << "\n高并发操作结果:" << std::endl;
    std::cout << "成功操作: " << update_success_count.load() << std::endl;
    std::cout << "失败操作: " << update_error_count.load() << std::endl;
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::cout << "总耗时: " << duration.count() << "ms" << std::endl;
}

int main()
{
    g_bbt_coroutine_config->m_cfg_stack_size = 32 * 1024;
    g_bbt_coroutine_config->m_cfg_max_coroutine = 1000;
    g_bbt_coroutine_config->m_cfg_static_thread_num = 6;
    g_scheduler->Start();
    bbt::core::thread::CountDownLatch latch{1};

    bbtco [&](){
        // 配置连接池
        MySQLPoolConfig config;
        config.host = "127.0.0.1";
        config.user = "root";
        config.password = "200101";
        config.database = "test_db";
        config.min_connections = 3;
        config.max_connections = 10;
        
        // 创建连接池和用户服务
        auto pool = std::make_shared<MySQLConnectionPool>(config);
        auto userService = std::make_shared<UserService>(pool);
    
        example1();
        example2(userService);
        example3(userService);

        latch.Down();
    };    

    latch.Wait();
    g_scheduler->Stop();

    return 0;
}