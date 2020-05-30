#include <boost/circular_buffer.hpp>
#include <mutex>
#include <vector>
#include <iostream>

template <typename T>
class circ_buffer
{
public:
    circ_buffer() {};
    circ_buffer(int n, T val) {
        buf.set_capacity(n);
        set_default(val);
    }
    void push (T data) {
        buf_mx.lock();
        // std::cout << "Lock acquired in push\n";
        buf.push_back(data);
        buf_mx.unlock();
        // std::cout << "    Lock released in push\n";
    }
    T pop() {
        T data;
        buf_mx.lock();
        // std::cout << "Lock acquired in pop\n";
        if (buf.empty()) {
            data = NULL;
        } else {
            data = buf.front();
            buf.pop_front();
        }
        buf_mx.unlock();
        // std::cout << "    Lock released in pop\n";
        return data;
    }
    void multipush(std::vector<T> data) {
        buf_mx.lock();
        // std::cout << "Lock acquired in multipush\n";
        buf.insert(buf.end(), data.begin(), data.end());
        buf_mx.unlock();
        // std::cout << "    Lock released in multipush\n";
    }
    std::vector<T> multipop() {
        std::vector<T> data;
        buf_mx.lock();
        // std::cout << "Lock acquired in multipop\n";
        data.insert(data.begin(), buf.begin(), buf.end());
        buf.clear();
        buf_mx.unlock();
        // std::cout << "    Lock released in multipop\n";
        return data;
    }
    std::vector<T> multipop(int n) {
        std::vector<T> data;
        while (buf.size() < n) {}
        data = multipop();
    }
    int size() {
        int size;
        size = buf.size();
        return buf.size();
    }
    void resize(int capacity, T val) {
        buf_mx.lock();
        // std::cout << "Lock acquired in resize (val)\n";
        buf.resize(capacity, val);
        buf_mx.unlock();
        // std::cout << "    Lock released in resize (val)\n";
    }
    void resize(int capacity) {
        buf_mx.lock();
        // std::cout << "Lock acquired in resize (no val)\n";
        buf.resize(capacity, default_value);
        buf_mx.unlock();
        // std::cout << "    Lock released in resize (no val)\n";
    }
    void set_capacity(int capacity) {
        buf_mx.lock();
        buf.set_capacity(capacity);
        buf_mx.unlock();
    }
    void set_default(T val) {
        default_value = val;
    }
private:
    std::mutex buf_mx;
    boost::circular_buffer<T> buf;
    T default_value = NULL;
};