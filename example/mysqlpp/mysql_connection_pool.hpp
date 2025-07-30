#pragma once

#include <bbt/coroutine/coroutine.hpp>
#include <mysql/mysql.h>
#include <memory>
#include <queue>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>

// 表示一行数据的键值对
using Row = std::unordered_map<std::string, std::string>;

// 表示查询结果的所有行
using ResultSet = std::vector<Row>;

// MySQL结果解析器
class MySQLResultParser {
public:
    // 解析MYSQL_RES为ResultSet
    static ResultSet ParseResult(MYSQL_RES* result) {
        if (!result) {
            return ResultSet{};
        }
        
        ResultSet resultSet;
        
        // 获取字段信息
        MYSQL_FIELD* fields = mysql_fetch_fields(result);
        unsigned int fieldCount = mysql_num_fields(result);
        
        // 创建字段名数组
        std::vector<std::string> fieldNames;
        fieldNames.reserve(fieldCount);
        for (unsigned int i = 0; i < fieldCount; ++i) {
            fieldNames.emplace_back(fields[i].name);
        }
        
        // 遍历所有行
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result))) {
            Row rowData;
            
            // 为每一列创建键值对
            for (unsigned int i = 0; i < fieldCount; ++i) {
                std::string value = (row[i] ? row[i] : ""); // NULL值转为空字符串
                rowData[fieldNames[i]] = value;
            }
            
            resultSet.push_back(std::move(rowData));
        }
        
        return resultSet;
    }
    
    // 打印ResultSet的内容
    static void PrintResultSet(const ResultSet& resultSet) {
        if (resultSet.empty()) {
            std::cout << "Empty result set." << std::endl;
            return;
        }
        
        // 打印表头
        const auto& firstRow = resultSet.front();
        for (const auto& [key, value] : firstRow) {
            std::cout << key << "\t";
        }
        std::cout << std::endl;
        
        // 打印分隔线
        for (const auto& [key, value] : firstRow) {
            std::cout << std::string(key.length(), '-') << "\t";
        }
        std::cout << std::endl;
        
        // 打印数据行
        for (const auto& row : resultSet) {
            for (const auto& [key, value] : firstRow) {
                auto it = row.find(key);
                if (it != row.end()) {
                    std::cout << it->second << "\t";
                } else {
                    std::cout << "NULL\t";
                }
            }
            std::cout << std::endl;
        }
    }
};

// MySQL连接包装类
class MySQLConnection {
public:
    MySQLConnection(const std::string& host, const std::string& user, 
                   const std::string& password, const std::string& database, 
                   unsigned int port = 3306)
        : m_host(host), m_user(user), m_password(password), 
          m_database(database), m_port(port), m_mysql(nullptr), m_connected(false) {
        Connect();
    }
    
    ~MySQLConnection() {
        Disconnect();
    }
    
    // 连接到数据库
    bool Connect() {
        if (m_connected) {
            return true;
        }
        
        m_mysql = mysql_init(nullptr);
        if (!m_mysql) {
            std::cerr << "mysql_init failed" << std::endl;
            return false;
        }
        
        // 设置连接超时
        unsigned int timeout = 10;
        mysql_options(m_mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
        
        if (!mysql_real_connect(m_mysql, m_host.c_str(), m_user.c_str(), 
                               m_password.c_str(), m_database.c_str(), 
                               m_port, nullptr, 0)) {
            std::cerr << "Connection failed: " << mysql_error(m_mysql) << std::endl;
            mysql_close(m_mysql);
            m_mysql = nullptr;
            return false;
        }
        
        m_connected = true;
        m_last_used = std::chrono::steady_clock::now();
        return true;
    }
    
    // 断开连接
    void Disconnect() {
        if (m_mysql) {
            mysql_close(m_mysql);
            m_mysql = nullptr;
        }
        m_connected = false;
    }
    
    // 检查连接是否有效
    bool IsConnected() const {
        return m_connected && m_mysql && mysql_ping(m_mysql) == 0;
    }
    
    // 执行查询
    ResultSet ExecuteQuery(const std::string& query) {
        if (!IsConnected()) {
            if (!Connect()) {
                throw std::runtime_error("Failed to connect to database");
            }
        }
        
        m_last_used = std::chrono::steady_clock::now();
        
        if (mysql_query(m_mysql, query.c_str())) {
            throw std::runtime_error(std::string("Query failed: ") + mysql_error(m_mysql));
        }
        
        MYSQL_RES* result = mysql_store_result(m_mysql);
        if (!result) {
            // 检查是否是不返回结果集的查询（如INSERT, UPDATE等）
            if (mysql_field_count(m_mysql) == 0) {
                // 这是一个不返回结果集的查询，返回空结果集
                return ResultSet{};
            } else {
                throw std::runtime_error(std::string("mysql_store_result failed: ") + mysql_error(m_mysql));
            }
        }
        
        ResultSet resultSet = MySQLResultParser::ParseResult(result);
        mysql_free_result(result);
        return resultSet;
    }
    
    // 执行非查询语句（INSERT, UPDATE, DELETE等）
    int ExecuteUpdate(const std::string& query) {
        if (!IsConnected()) {
            if (!Connect()) {
                throw std::runtime_error("Failed to connect to database");
            }
        }
        
        m_last_used = std::chrono::steady_clock::now();
        
        if (mysql_query(m_mysql, query.c_str())) {
            throw std::runtime_error(std::string("Query failed: ") + mysql_error(m_mysql));
        }
        
        return mysql_affected_rows(m_mysql);
    }
    
    // 获取最后使用时间
    std::chrono::steady_clock::time_point GetLastUsed() const {
        return m_last_used;
    }
    
    // 获取原始MYSQL指针
    MYSQL* GetMysql() const { 
        return m_mysql; 
    }

private:
    std::string m_host;
    std::string m_user;
    std::string m_password;
    std::string m_database;
    unsigned int m_port;
    MYSQL* m_mysql;
    bool m_connected;
    std::chrono::steady_clock::time_point m_last_used;
};

// MySQL连接池配置
struct MySQLPoolConfig {
    std::string host = "127.0.0.1";
    std::string user = "root";
    std::string password = "";
    std::string database = "test";
    unsigned int port = 3306;
    
    size_t min_connections = 2;     // 最小连接数
    size_t max_connections = 10;    // 最大连接数
    size_t idle_timeout_seconds = 300; // 空闲连接超时时间（秒）
    size_t connection_timeout_seconds = 30; // 获取连接超时时间（秒）
};

// 基于协程的MySQL连接池
class MySQLConnectionPool {
public:
    using ConnectionPtr = std::shared_ptr<MySQLConnection>;
    
    explicit MySQLConnectionPool(const MySQLPoolConfig& config)
        : m_config(config), m_shutdown(false), m_active_connections(0) {
        
        // 创建协程互斥锁
        m_mutex = bbt::co::sync::CoMutex::Create();
        
        // 初始化最小连接数
        InitializeConnections();
        
        // 启动连接管理协程
        StartManagementCoroutine();
    }
    
    ~MySQLConnectionPool() {
        Shutdown();
    }
    
    // 获取连接（协程安全）
    ConnectionPtr GetConnection(int timeout_ms = 30000) {
        if (m_shutdown) {
            throw std::runtime_error("Connection pool is shutdown");
        }
        
        // 等待获取连接，支持超时
        auto timeout_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        
        while (std::chrono::steady_clock::now() < timeout_time) {
            m_mutex->Lock();
            
            if (!m_connection_queue.empty()) {
                ConnectionPtr conn = m_connection_queue.front();
                m_connection_queue.pop();
                m_mutex->UnLock();
                
                // 检查连接是否有效
                if (conn && conn->IsConnected()) {
                    return conn;
                } else {
                    // 连接无效，创建新连接
                    conn = CreateConnection();
                    if (conn) {
                        return conn;
                    }
                }
            } else {
                // 连接池为空，尝试创建新连接
                if (m_active_connections < m_config.max_connections) {
                    m_active_connections++;
                    m_mutex->UnLock();
                    
                    ConnectionPtr conn = CreateConnection();
                    if (conn) {
                        return conn;
                    } else {
                        // 创建失败，减少活跃连接计数
                        m_mutex->Lock();
                        m_active_connections--;
                        m_mutex->UnLock();
                    }
                } else {
                    m_mutex->UnLock();
                }
            }
            
            // 等待一小段时间后重试
            bbtco_sleep(100); // 等待100ms
        }
        
        throw std::runtime_error("Get connection timeout");
    }
    
    // 归还连接
    void ReturnConnection(ConnectionPtr conn) {
        if (m_shutdown || !conn) {
            return;
        }
        
        // 检查连接是否还有效
        if (conn->IsConnected()) {
            m_mutex->Lock();
            m_connection_queue.push(conn);
            m_mutex->UnLock();
        } else {
            // 连接无效，减少活跃连接计数
            m_mutex->Lock();
            m_active_connections--;
            m_mutex->UnLock();
        }
    }
    
    // 关闭连接池
    void Shutdown() {
        m_shutdown = true;
        m_mutex->Lock();
        while (!m_connection_queue.empty()) {
            auto conn = m_connection_queue.front();
            m_connection_queue.pop();
            if (conn && conn->IsConnected()) {
                conn->Disconnect(); // 断开连接
            }
        }
        m_mutex->UnLock();
    }
    
    // 获取连接池状态
    void PrintStatus() {
        m_mutex->Lock();
        std::cout << "=== MySQL Connection Pool Status ===" << std::endl;
        std::cout << "Max connections: " << m_config.max_connections << std::endl;
        std::cout << "Min connections: " << m_config.min_connections << std::endl;
        std::cout << "Active connections: " << m_active_connections << std::endl;
        std::cout << "Available connections: " << m_connection_queue.size() << std::endl;
        std::cout << "Shutdown: " << (m_shutdown ? "Yes" : "No") << std::endl;
        m_mutex->UnLock();
    }

private:
    // 创建新连接
    ConnectionPtr CreateConnection() {
        try {
            return std::make_shared<MySQLConnection>(
                m_config.host, m_config.user, m_config.password, 
                m_config.database, m_config.port
            );
        } catch (const std::exception& e) {
            std::cerr << "Failed to create connection: " << e.what() << std::endl;
            return nullptr;
        }
    }
    
    // 初始化最小连接数
    void InitializeConnections() {
        for (size_t i = 0; i < m_config.min_connections; ++i) {
            auto conn = CreateConnection();
            if (conn) {
                m_mutex->Lock();
                m_connection_queue.push(conn);
                m_active_connections++;
                m_mutex->UnLock();
            }
        }
    }
    
    // 启动连接管理协程
    void StartManagementCoroutine() {
        bbtco_desc("mysql_pool_manager") [this]() {
            while (!m_shutdown) {
                // 定期检查连接池状态
                bbtco_sleep(10000); // 每10秒检查一次
                
                if (m_shutdown) {
                    break;
                }
                
                // 维护连接池
                MaintainConnectionPool();
            }
        };
    }
    
    // 维护连接池状态
    void MaintainConnectionPool() {
        m_mutex->Lock();
        
        // 清理无效连接
        std::queue<ConnectionPtr> valid_connections;
        size_t removed_count = 0;
        
        while (!m_connection_queue.empty()) {
            auto conn = m_connection_queue.front();
            m_connection_queue.pop();
            
            if (conn && conn->IsConnected()) {
                // 检查连接是否超时
                auto now = std::chrono::steady_clock::now();
                auto idle_time = std::chrono::duration_cast<std::chrono::seconds>(
                    now - conn->GetLastUsed()).count();
                    
                if (idle_time < static_cast<long>(m_config.idle_timeout_seconds)) {
                    valid_connections.push(conn);
                } else {
                    // 连接超时，断开连接
                    conn->Disconnect();
                    removed_count++;
                }
            } else {
                // 无效连接
                removed_count++;
            }
        }
        
        // 更新连接队列和活跃连接计数
        m_connection_queue = std::move(valid_connections);
        m_active_connections -= removed_count;
        
        // 确保最小连接数
        size_t current_total = m_connection_queue.size() + 
            (m_active_connections - m_connection_queue.size());
        
        if (current_total < m_config.min_connections) {
            size_t need_create = m_config.min_connections - current_total;
            m_mutex->UnLock();

            // 创建需要的连接
            for (size_t i = 0; i < need_create && !m_shutdown; ++i) {
                auto conn = CreateConnection();
                if (conn) {
                    m_mutex->Lock();
                    m_connection_queue.push(conn);
                    m_active_connections++;
                    m_mutex->UnLock();
                }
            }
        } else {
            m_mutex->UnLock();
        }
        
        std::cout << "Connection pool maintenance completed. "
                  << "Removed " << removed_count << " invalid/expired connections." << std::endl;
    }

private:
    MySQLPoolConfig m_config;
    std::queue<ConnectionPtr> m_connection_queue;
    size_t m_active_connections;
    bbt::co::sync::CoMutex::SPtr m_mutex;
    bool m_shutdown;
};

// RAII连接管理类
class MySQLConnectionGuard {
public:
    MySQLConnectionGuard(MySQLConnectionPool& pool, int timeout_ms = 30000)
        : m_pool(pool), m_connection(pool.GetConnection(timeout_ms)) {
    }
    
    ~MySQLConnectionGuard() {
        if (m_connection) {
            m_pool.ReturnConnection(m_connection);
        }
    }
    
    MySQLConnection* operator->() {
        return m_connection.get();
    }
    
    MySQLConnection& operator*() {
        return *m_connection;
    }
    
    bool IsValid() const {
        return m_connection && m_connection->IsConnected();
    }

private:
    MySQLConnectionPool& m_pool;
    MySQLConnectionPool::ConnectionPtr m_connection;
};
