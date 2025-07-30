# MySQL连接池 - 基于bbtools-coroutine协程库

这是一个基于bbtools-coroutine协程库实现的高性能MySQL连接池，支持协程安全的数据库操作。

## 特性

- **协程安全**: 使用协程通道(Chan)管理连接池，避免传统锁机制的性能开销
- **自动连接管理**: 支持连接的自动创建、重连和清理
- **RAII管理**: 提供RAII风格的连接管理，自动归还连接到池中
- **高并发支持**: 支持大量协程同时访问数据库
- **连接池配置**: 支持最小/最大连接数、超时时间等配置
- **错误处理**: 完善的异常处理机制

## 核心组件

### 1. MySQLConnection
基础的MySQL连接包装类，提供：
- 自动重连机制
- 连接状态检查
- 查询和更新操作
- 结果集解析

### 2. MySQLConnectionPool
协程安全的连接池实现：
- 使用`sync::Chan`管理可用连接
- 支持连接超时机制
- 自动维护最小连接数
- 连接健康检查

### 3. MySQLConnectionGuard
RAII风格的连接管理器：
- 自动获取连接
- 作用域结束时自动归还连接
- 异常安全

### 4. MySQLResultParser
结果集解析工具：
- 将MYSQL_RES转换为易用的数据结构
- 支持结果集打印
- NULL值处理

## 使用示例

### 基本使用

```cpp
#include <bbt/coroutine/coroutine.hpp>
#include "mysql_connection_pool.hpp"

int main() {
    // 启动协程调度器
    g_scheduler->Start();
    
    // 配置连接池
    MySQLPoolConfig config;
    config.host = "127.0.0.1";
    config.user = "root";
    config.password = "your_password";
    config.database = "your_database";
    config.min_connections = 2;
    config.max_connections = 10;
    
    // 创建连接池
    auto pool = std::make_shared<MySQLConnectionPool>(config);
    
    // 在协程中使用连接池
    bbtco [pool]() {
        try {
            // 使用RAII方式获取连接
            MySQLConnectionGuard conn(*pool);
            
            if (conn.IsValid()) {
                // 执行查询
                ResultSet result = conn->ExecuteQuery("SELECT * FROM users LIMIT 10");
                MySQLResultParser::PrintResultSet(result);
                
                // 执行更新
                int affected = conn->ExecuteUpdate("UPDATE users SET status = 1 WHERE id = 1");
                std::cout << "影响行数: " << affected << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "数据库操作失败: " << e.what() << std::endl;
        }
    };
    
    // 等待协程完成
    sleep(1);
    
    // 关闭调度器
    g_scheduler->Stop();
    return 0;
}
```

### 高并发示例

```cpp
// 创建多个协程同时访问数据库
bbt::core::thread::CountDownLatch latch{10};

for (int i = 0; i < 10; ++i) {
    bbtco [pool, &latch, i]() {
        try {
            MySQLConnectionGuard conn(*pool, 5000); // 5秒超时
            
            if (conn.IsValid()) {
                // 每个协程执行不同的查询
                std::string sql = "SELECT * FROM users WHERE id = " + std::to_string(i);
                ResultSet result = conn->ExecuteQuery(sql);
                
                std::cout << "协程 " << i << " 查询完成，结果数: " << result.size() << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "协程 " << i << " 错误: " << e.what() << std::endl;
        }
        
        latch.Down();
    };
}

latch.Wait(); // 等待所有协程完成
```

## 编译和运行

### 依赖项
- bbtools-coroutine 协程库
- MySQL C API (libmysqlclient-dev)
- CMake 3.16+
- C++17 编译器

### 编译步骤

```bash
# 在项目根目录下
mkdir build && cd build
cmake ..
make

# 运行示例
./bin/example/test_connection_pool          # 基础连接池测试
./bin/example/advanced_pool_example         # 高级示例（包含用户服务）
```

### 数据库准备

```sql
-- 创建测试数据库
CREATE DATABASE test_db;
USE test_db;

-- 创建测试表
CREATE TABLE log_data (
    id INT AUTO_INCREMENT PRIMARY KEY,
    message TEXT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- 插入测试数据
INSERT INTO log_data (message) VALUES 
('Test message 1'),
('Test message 2'),
('Test message 3'),
('Test message 4'),
('Test message 5');
```

## 配置参数

### MySQLPoolConfig

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| host | string | "127.0.0.1" | MySQL服务器地址 |
| user | string | "root" | 用户名 |
| password | string | "" | 密码 |
| database | string | "test" | 数据库名 |
| port | uint | 3306 | 端口号 |
| min_connections | size_t | 2 | 最小连接数 |
| max_connections | size_t | 10 | 最大连接数 |
| idle_timeout_seconds | size_t | 300 | 空闲连接超时（秒） |
| connection_timeout_seconds | size_t | 30 | 获取连接超时（秒） |

## 最佳实践

1. **合理设置连接池大小**: 根据应用的并发量和数据库性能来设置最小和最大连接数

2. **使用RAII管理连接**: 始终使用`MySQLConnectionGuard`来自动管理连接的获取和归还

3. **异常处理**: 在数据库操作周围使用try-catch块来处理可能的异常

4. **避免长时间占用连接**: 不要在获取连接后执行长时间的非数据库操作

5. **连接超时设置**: 根据业务需求设置合适的连接获取超时时间

## 注意事项

- 确保MySQL服务器支持所需的并发连接数
- 在高并发场景下，注意监控数据库的连接数和性能
- 连接池在程序退出时会自动清理所有连接
- 建议在生产环境中启用MySQL的慢查询日志来监控性能

## 示例程序说明

1. **co_with_mysqlpp.cc**: 原始的单连接MySQL示例
2. **test_connection_pool.cc**: 连接池基础功能测试
3. **advanced_pool_example.cc**: 包含用户服务的完整示例，演示CRUD操作和高并发场景

通过这些示例，你可以了解如何在实际项目中使用基于协程的MySQL连接池。
