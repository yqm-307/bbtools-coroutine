#pragma once
#include <bbt/coroutine/coroutine.hpp>
#include "mysql_connection_pool.hpp"
#include <iostream>
#include <memory>
#include <atomic>
#include <random>

// 模拟用户数据结构
struct User {
    int id;
    std::string name;
    std::string email;
    int age;
};

// 用户服务类，演示如何在实际应用中使用连接池
class UserService {
public:
    explicit UserService(std::shared_ptr<MySQLConnectionPool> pool) 
        : m_pool(pool) {}
    
    // 创建用户表（如果不存在）
    bool CreateUserTable() {
        if (!m_pool) return false;
        
        try {
            MySQLConnectionGuard conn(*m_pool);
            if (!conn.IsValid()) {
                std::cerr << "无法获取数据库连接" << std::endl;
                return false;
            }
            
            std::string create_sql = R"(
                CREATE TABLE IF NOT EXISTS users (
                    id INT AUTO_INCREMENT PRIMARY KEY,
                    name VARCHAR(100) NOT NULL,
                    email VARCHAR(100) UNIQUE NOT NULL,
                    age INT NOT NULL,
                    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
                ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
            )";
            
            conn->ExecuteUpdate(create_sql);
            std::cout << "用户表创建成功或已存在" << std::endl;
            return true;
            
        } catch (const std::exception& e) {
            std::cerr << "创建用户表失败: " << e.what() << std::endl;
            return false;
        }
    }
    
    void ClearTable()
    {
        if (!m_pool) return;
        
        try {
            MySQLConnectionGuard conn(*m_pool);
            if (!conn.IsValid()) {
                std::cerr << "无法获取数据库连接" << std::endl;
                return;
            }
            
            conn->ExecuteUpdate("TRUNCATE TABLE users");
            std::cout << "用户表已清空" << std::endl;
            
        } catch (const std::exception& e) {
            std::cerr << "清空用户表失败: " << e.what() << std::endl;
        }
    }


    // 插入用户
    bool InsertUser(const User& user) {
        if (!m_pool) return false;
        
        try {
            MySQLConnectionGuard conn(*m_pool);
            if (!conn.IsValid()) {
                return false;
            }
            
            std::string sql = "INSERT INTO users (name, email, age) VALUES ('" +
                             user.name + "', '" + user.email + "', " + 
                             std::to_string(user.age) + ")";
            
            int affected = conn->ExecuteUpdate(sql);
            std::cout << "插入用户成功: " << user.name << ", 影响行数: " << affected << std::endl;
            return affected > 0;
            
        } catch (const std::exception& e) {
            std::cerr << "插入用户失败: " << e.what() << std::endl;
            return false;
        }
    }
    
    // 查询所有用户
    std::vector<User> GetAllUsers() {
        std::vector<User> users;
        if (!m_pool) return users;
        
        try {
            MySQLConnectionGuard conn(*m_pool);
            if (!conn.IsValid()) {
                return users;
            }
            
            ResultSet result = conn->ExecuteQuery("SELECT id, name, email, age FROM users ORDER BY id");
            
            for (const auto& row : result) {
                User user;
                user.id = std::stoi(row.at("id"));
                user.name = row.at("name");
                user.email = row.at("email");
                user.age = std::stoi(row.at("age"));
                users.push_back(user);
            }
            
            std::cout << "查询到 " << users.size() << " 个用户" << std::endl;
            
        } catch (const std::exception& e) {
            std::cerr << "查询用户失败: " << e.what() << std::endl;
        }
        
        return users;
    }
    
    // 根据ID查询用户
    bool GetUserById(int id, User& user) {
        if (!m_pool) return false;
        
        try {
            MySQLConnectionGuard conn(*m_pool);
            if (!conn.IsValid()) {
                return false;
            }
            
            std::string sql = "SELECT id, name, email, age FROM users WHERE id = " + std::to_string(id);
            ResultSet result = conn->ExecuteQuery(sql);
            
            if (!result.empty()) {
                const auto& row = result[0];
                user.id = std::stoi(row.at("id"));
                user.name = row.at("name");
                user.email = row.at("email");
                user.age = std::stoi(row.at("age"));
                return true;
            }
            
        } catch (const std::exception& e) {
            std::cerr << "根据ID查询用户失败: " << e.what() << std::endl;
        }
        
        return false;
    }
    
    // 更新用户
    bool UpdateUser(const User& user) {
        if (!m_pool) return false;
        
        try {
            MySQLConnectionGuard conn(*m_pool);
            if (!conn.IsValid()) {
                return false;
            }
            
            std::string sql = "UPDATE users SET name = '" + user.name + 
                             "', email = '" + user.email + 
                             "', age = " + std::to_string(user.age) + 
                             " WHERE id = " + std::to_string(user.id);
            
            int affected = conn->ExecuteUpdate(sql);
            return affected > 0;
            
        } catch (const std::exception& e) {
            std::cerr << "更新用户失败: " << e.what() << std::endl;
            return false;
        }
    }
    
    // 删除用户
    bool DeleteUser(int id) {
        if (!m_pool) return false;
        
        try {
            MySQLConnectionGuard conn(*m_pool);
            if (!conn.IsValid()) {
                return false;
            }
            
            std::string sql = "DELETE FROM users WHERE id = " + std::to_string(id);
            int affected = conn->ExecuteUpdate(sql);
            std::cout << "删除用户成功, 影响行数: " << affected << std::endl;
            return affected > 0;
            
        } catch (const std::exception& e) {
            std::cerr << "删除用户失败: " << e.what() << std::endl;
            return false;
        }
    }

private:
    std::shared_ptr<MySQLConnectionPool> m_pool;
};