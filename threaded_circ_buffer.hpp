#include <boost/circular_buffer.hpp>
#include <mutex>
#include <vector>
#include <iostream>

template <typename T>
class threaded_circ_buffer
{
public:
    threaded_circ_buffer() {};
    threaded_circ_buffer(int n, T val) {
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
    std::vector<T> multipop(int n=0) {
        std::vector<T> data;
        buf_mx.lock();
        // std::cout << "Lock acquired in multipop\n";
        if (n == 0 || buf.size() == n) {
            data.insert(data.begin(), buf.begin(), buf.end());
            buf.clear();
        } else if (buf.size() > n) {
            data.insert(data.begin(), buf.begin(), buf.begin()+n);
            buf.erase_begin(n);
        }
        buf_mx.unlock();
        // std::cout << "    Lock released in multipop\n";
        return data;
    }
    int size() {
        return buf.size();
    }
    int capacity() {
        return buf.capacity();
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