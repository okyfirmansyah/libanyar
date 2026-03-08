// LibAnyar — Database Plugin Implementation
// Uses LibAsyik's SOCI pool for fiber-safe database access.

#include <anyar/plugins/db_plugin.h>

#include <libasyik/sql.hpp>

#include <iostream>
#include <random>
#include <sstream>

namespace anyar {

// ── Helpers ─────────────────────────────────────────────────────────────────

std::string DbPlugin::generate_handle() {
    static std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream ss;
    ss << "db_" << std::hex << dist(gen);
    return ss.str();
}

asyik::sql_pool_ptr DbPlugin::get_pool(const std::string& handle) {
    std::lock_guard<boost::fibers::mutex> lock(pools_mutex_);
    auto it = pools_.find(handle);
    if (it == pools_.end()) {
        throw std::runtime_error("Unknown database handle: " + handle);
    }
    return it->second;
}

// Convert a SOCI row to a JSON object using column metadata
static json row_to_json(const soci::row& r) {
    json obj = json::object();
    for (std::size_t i = 0; i < r.size(); ++i) {
        const auto& props = r.get_properties(i);
        const std::string& col = props.get_name();

        if (r.get_indicator(i) == soci::i_null) {
            obj[col] = nullptr;
            continue;
        }

        switch (props.get_data_type()) {
            case soci::dt_string:
                obj[col] = r.get<std::string>(i);
                break;
            case soci::dt_double:
                obj[col] = r.get<double>(i);
                break;
            case soci::dt_integer:
                obj[col] = r.get<int>(i);
                break;
            case soci::dt_long_long:
                obj[col] = r.get<long long>(i);
                break;
            case soci::dt_unsigned_long_long:
                obj[col] = r.get<unsigned long long>(i);
                break;
            case soci::dt_date: {
                std::tm tm = r.get<std::tm>(i);
                char buf[64];
                std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
                obj[col] = std::string(buf);
                break;
            }
            default:
                // Fallback: try as string
                try {
                    obj[col] = r.get<std::string>(i);
                } catch (...) {
                    obj[col] = nullptr;
                }
                break;
        }
    }
    return obj;
}

// Extract column names from a SOCI row
static json columns_from_row(const soci::row& r) {
    json cols = json::array();
    for (std::size_t i = 0; i < r.size(); ++i) {
        cols.push_back(r.get_properties(i).get_name());
    }
    return cols;
}

// Bind positional parameters to a SOCI statement string
// Replaces $1, $2, ... with :p1, :p2, ... for named binding
// and prepares the use() calls
static std::string rewrite_params(const std::string& sql) {
    // SOCI uses :name syntax. We support $1, $2, ... (positional) from the JS side.
    // Rewrite $N → :pN
    std::string result;
    result.reserve(sql.size() + 16);
    for (size_t i = 0; i < sql.size(); ++i) {
        if (sql[i] == '$' && i + 1 < sql.size() && std::isdigit(sql[i + 1])) {
            result += ":p";
            ++i;
            while (i < sql.size() && std::isdigit(sql[i])) {
                result += sql[i];
                ++i;
            }
            --i; // will be incremented by for loop
        } else {
            result += sql[i];
        }
    }
    return result;
}

// Execute SQL with positional params using a SOCI session on a background thread
// For SELECT queries — returns rows + columns
static json exec_query(asyik::service_ptr svc, asyik::sql_session_ptr ses,
                        const std::string& sql, const json& params) {
    std::string rewritten = rewrite_params(sql);
    json rows_arr = json::array();
    json cols_arr = json::array();

    svc->async([&]() {
        soci::session& soc = *ses->soci_session;

        // Prepare statement — use exchange() for both into and use bindings
        // so that define_and_bind() processes them all uniformly.
        soci::row r;
        soci::statement st = (soc.prepare << rewritten);
        st.exchange(soci::into(r));

        // Bind positional parameters
        // SOCI named parameters: :p1, :p2, ...
        // We store them as local variables so they outlive the execute
        std::vector<std::string> str_vals;
        std::vector<int> int_vals;
        std::vector<double> dbl_vals;
        std::vector<soci::indicator> indicators;
        str_vals.reserve(params.size());
        int_vals.reserve(params.size());
        dbl_vals.reserve(params.size());
        indicators.reserve(params.size());

        for (size_t i = 0; i < params.size(); ++i) {
            std::string pname = "p" + std::to_string(i + 1);
            const auto& p = params[i];
            if (p.is_string()) {
                str_vals.push_back(p.get<std::string>());
                indicators.push_back(soci::i_ok);
                st.exchange(soci::use(str_vals.back(), indicators.back(), pname));
            } else if (p.is_number_integer()) {
                int_vals.push_back(p.get<int>());
                indicators.push_back(soci::i_ok);
                st.exchange(soci::use(int_vals.back(), indicators.back(), pname));
            } else if (p.is_number_float()) {
                dbl_vals.push_back(p.get<double>());
                indicators.push_back(soci::i_ok);
                st.exchange(soci::use(dbl_vals.back(), indicators.back(), pname));
            } else if (p.is_null()) {
                // Use an indicator for NULL
                str_vals.push_back("");
                indicators.push_back(soci::i_null);
                st.exchange(soci::use(str_vals.back(), indicators.back(), pname));
            } else {
                // Serialize as string
                str_vals.push_back(p.dump());
                indicators.push_back(soci::i_ok);
                st.exchange(soci::use(str_vals.back(), indicators.back(), pname));
            }
        }

        st.define_and_bind();
        st.execute(true); // true = fetch first row

        bool has_rows = st.got_data();
        if (has_rows) {
            cols_arr = columns_from_row(r);
            rows_arr.push_back(row_to_json(r));
            while (st.fetch()) {
                rows_arr.push_back(row_to_json(r));
            }
        }
    }).get();

    return {{"rows", rows_arr}, {"columns", cols_arr}};
}

// Execute non-SELECT SQL — returns affected row count
static json exec_statement(asyik::service_ptr svc, asyik::sql_session_ptr ses,
                            const std::string& sql, const json& params) {
    std::string rewritten = rewrite_params(sql);
    int affected = 0;

    svc->async([&]() {
        soci::session& soc = *ses->soci_session;

        soci::statement st = (soc.prepare << rewritten);

        std::vector<std::string> str_vals;
        std::vector<int> int_vals;
        std::vector<double> dbl_vals;
        std::vector<soci::indicator> indicators;
        str_vals.reserve(params.size());
        int_vals.reserve(params.size());
        dbl_vals.reserve(params.size());
        indicators.reserve(params.size());

        for (size_t i = 0; i < params.size(); ++i) {
            std::string pname = "p" + std::to_string(i + 1);
            const auto& p = params[i];
            if (p.is_string()) {
                str_vals.push_back(p.get<std::string>());
                indicators.push_back(soci::i_ok);
                st.exchange(soci::use(str_vals.back(), indicators.back(), pname));
            } else if (p.is_number_integer()) {
                int_vals.push_back(p.get<int>());
                indicators.push_back(soci::i_ok);
                st.exchange(soci::use(int_vals.back(), indicators.back(), pname));
            } else if (p.is_number_float()) {
                dbl_vals.push_back(p.get<double>());
                indicators.push_back(soci::i_ok);
                st.exchange(soci::use(dbl_vals.back(), indicators.back(), pname));
            } else if (p.is_null()) {
                str_vals.push_back("");
                indicators.push_back(soci::i_null);
                st.exchange(soci::use(str_vals.back(), indicators.back(), pname));
            } else {
                str_vals.push_back(p.dump());
                indicators.push_back(soci::i_ok);
                st.exchange(soci::use(str_vals.back(), indicators.back(), pname));
            }
        }

        st.define_and_bind();
        st.execute(true);
        affected = st.get_affected_rows();
    }).get();

    return {{"affectedRows", affected}};
}

// ── Plugin Initialization ───────────────────────────────────────────────────

void DbPlugin::initialize(PluginContext& ctx) {
    service_ = ctx.service;
    auto& cmds = ctx.commands;

    // ── db:open ─────────────────────────────────────────────────────────────
    cmds.add("db:open", [this](const json& args) -> json {
        std::string backend = args.at("backend").get<std::string>();
        std::string conn_str = args.at("connStr").get<std::string>();
        int pool_size = args.value("poolSize", 4);

        if (pool_size < 1) pool_size = 1;
        if (pool_size > 32) pool_size = 32;

        asyik::sql_pool_ptr pool;
        if (backend == "sqlite3") {
            pool = asyik::make_sql_pool(soci::sqlite3, conn_str,
                                         static_cast<size_t>(pool_size));
        } else if (backend == "postgresql") {
            pool = asyik::make_sql_pool(soci::postgresql, conn_str,
                                         static_cast<size_t>(pool_size));
        } else {
            throw std::runtime_error("Unsupported database backend: " + backend +
                                     ". Use 'sqlite3' or 'postgresql'.");
        }

        std::string handle = generate_handle();
        {
            std::lock_guard<boost::fibers::mutex> lock(pools_mutex_);
            pools_[handle] = pool;
        }

        std::cout << "[DbPlugin] Opened " << backend << " database: "
                  << conn_str << " (handle=" << handle
                  << ", pool=" << pool_size << ")" << std::endl;

        return handle;
    });

    // ── db:close ────────────────────────────────────────────────────────────
    cmds.add("db:close", [this](const json& args) -> json {
        std::string handle = args.at("handle").get<std::string>();
        {
            std::lock_guard<boost::fibers::mutex> lock(pools_mutex_);
            auto it = pools_.find(handle);
            if (it == pools_.end()) {
                throw std::runtime_error("Unknown database handle: " + handle);
            }
            pools_.erase(it);
        }
        std::cout << "[DbPlugin] Closed database handle=" << handle << std::endl;
        return {{"closed", true}};
    });

    // ── db:query — SELECT queries, returns rows + columns ───────────────────
    cmds.add("db:query", [this](const json& args) -> json {
        std::string handle = args.at("handle").get<std::string>();
        std::string sql = args.at("sql").get<std::string>();
        json params = args.value("params", json::array());

        auto pool = get_pool(handle);
        auto ses = pool->get_session(service_);
        return exec_query(service_, ses, sql, params);
    });

    // ── db:exec — INSERT/UPDATE/DELETE, returns affectedRows ────────────────
    cmds.add("db:exec", [this](const json& args) -> json {
        std::string handle = args.at("handle").get<std::string>();
        std::string sql = args.at("sql").get<std::string>();
        json params = args.value("params", json::array());

        auto pool = get_pool(handle);
        auto ses = pool->get_session(service_);
        return exec_statement(service_, ses, sql, params);
    });

    // ── db:batch — Multiple statements in a transaction ─────────────────────
    cmds.add("db:batch", [this](const json& args) -> json {
        std::string handle = args.at("handle").get<std::string>();
        json statements = args.at("statements");

        auto pool = get_pool(handle);
        auto ses = pool->get_session(service_);

        json results = json::array();

        // Use LibAsyik's transaction wrapper
        asyik::sql_transaction txn(ses);
        try {
            for (const auto& stmt : statements) {
                std::string sql = stmt.at("sql").get<std::string>();
                json params = stmt.value("params", json::array());
                results.push_back(exec_statement(service_, ses, sql, params));
            }
            txn.commit();
        } catch (...) {
            // Transaction auto-rolls back on destruction if not committed
            throw;
        }

        return results;
    });

    std::cout << "[DbPlugin] Initialized — db:open, db:close, db:query, db:exec, db:batch" << std::endl;
}

void DbPlugin::shutdown() {
    std::lock_guard<boost::fibers::mutex> lock(pools_mutex_);
    pools_.clear();
    std::cout << "[DbPlugin] All database pools released" << std::endl;
}

} // namespace anyar
