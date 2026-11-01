// The earlier order book (order_book.cpp, June 2025), copied unchanged except:
//   - wrapped in namespace legacy, with #pragma once, and main() removed;
//   - find_order() added at the end of Orderbook, so the ITCH adapter can
//     read an order's side and shares.
// It is the baseline the new books are measured against (results/book.md).
#pragma once

#include <map>
#include <unordered_map>
#include <memory>

namespace legacy {

class Order;
class Dlist;
class Orderbook;

enum class Ordertype
{
    BUY,
    SELL
};

class Order
{
private:
    std::weak_ptr<Order> prev, next;
    friend Dlist;

public:
    size_t orderd_id;
    double price;
    size_t volume;
    Ordertype ordertype;

    Order(size_t id, double pric, size_t quantity, Ordertype type) : orderd_id{id},
                                                                     price{pric},
                                                                     volume{quantity},
                                                                     ordertype{type},
                                                                     prev{},
                                                                     next{}
    {
    }

    inline void update_volume(size_t new_volume)
    {
        volume = new_volume;
    }

    inline void update_price(double new_price)
    {
        price = new_price;
    }
};

class Dlist
{

public:
    std::shared_ptr<Order> head, tail;
    double price;
    size_t volume;
    size_t size;

    Dlist(double pric) : price{pric},
                         volume{0},
                         size{0},
                         head{},
                         tail{}
    {
    }

    void add_order(std::shared_ptr<Order> &order)
    {
        if (size == 0)
        {
            tail = head = order;
        }
        else
        {
            tail->next = order;
            order->prev = tail;
            tail = order;
        }
        size++;
        volume += order->volume;
    }

    void remove_order(std::shared_ptr<Order> &order)
    {
        if (size == 1)
        {
            tail = head = nullptr;
        }
        else
        {
            if (order == head)
            {
                if (auto new_head = head->next.lock())
                {
                    head = new_head;
                    head->prev.reset();
                }
            }
            else if (order == tail)
            {
                if (auto new_tail = tail->prev.lock())
                {
                    tail = new_tail;
                    tail->next.reset();
                }
            }
            else
            {
                if (auto prev_order = order->prev.lock())
                {
                    prev_order->next = order->next;
                }
                if (auto next_order = order->next.lock())
                {
                    next_order->prev = order->prev;
                }
            }
        }
        order->prev.reset();
        order->next.reset();
        size--;
        volume -= order->volume;
    }

    void update_order_volume(std::shared_ptr<Order> &order, int new_volume)
    {
        volume += new_volume - order->volume;
        order->update_volume(new_volume);
    }
};

class Orderbook
{
private:
    std::unordered_map<size_t, std::shared_ptr<Order>> order_map;
    std::map<double, std::shared_ptr<Dlist>, std::greater<double>> buy_orders;
    std::map<double, std::shared_ptr<Dlist>> sell_orders;
    size_t total_buy_orders, total_sell_orders;

    void match_buy_order(std::shared_ptr<Order> &order)
    {
        double price = order->price;
        while (order->volume > 0 && total_sell_orders > 0 && sell_orders.begin()->first <= price)
        {
            std::shared_ptr<Dlist> dlist = sell_orders.begin()->second;
            std::shared_ptr<Order> trade_order = dlist->head;
            double trade_price = trade_order->price;
            size_t trade_volume = std::min(trade_order->volume, order->volume);
            order->volume -= trade_volume;
            dlist->update_order_volume(trade_order, trade_order->volume - trade_volume);
            if (trade_order->volume == 0)
            {
                total_sell_orders--;
                dlist->remove_order(trade_order);
                order_map.erase(trade_order->orderd_id);
                if (dlist->size == 0)
                {
                    sell_orders.erase(trade_price);
                }
            }
        }
    }

    void match_sell_order(std::shared_ptr<Order> &order)
    {
        double price = order->price;
        while (order->volume > 0 && total_buy_orders > 0 && buy_orders.begin()->first >= price)
        {
            std::shared_ptr<Dlist> dlist = buy_orders.begin()->second;
            std::shared_ptr<Order> trade_order = dlist->head;
            double trade_price = trade_order->price;
            size_t trade_volume = std::min(trade_order->volume, order->volume);
            order->volume -= trade_volume;
            dlist->update_order_volume(trade_order, trade_order->volume - trade_volume);
            if (trade_order->volume == 0)
            {
                total_buy_orders--;
                dlist->remove_order(trade_order);
                order_map.erase(trade_order->orderd_id);
                if (dlist->size == 0)
                {
                    buy_orders.erase(trade_price);
                }
            }
        }
    }
    void process_order(std::shared_ptr<Order> &order)
    {
        if (order->ordertype == Ordertype::BUY)
        {
            match_buy_order(order);
            if (order->volume > 0)
            {
                if (buy_orders.find(order->price) == buy_orders.end())
                {
                    buy_orders[order->price] = std::make_shared<Dlist>(order->price);
                }
                buy_orders[order->price]->add_order(order);

                if (order_map.find(order->orderd_id) == order_map.end())
                {
                    order_map[order->orderd_id] = order;
                    total_buy_orders++;
                }
            }
            else
            {
                if (order_map.find(order->orderd_id) != order_map.end())
                {
                    order_map.erase(order->orderd_id);
                    total_buy_orders--;
                }
            }
        }
        else
        {
            match_sell_order(order);
            if (order->volume > 0)
            {
                if (sell_orders.find(order->price) == sell_orders.end())
                {
                    sell_orders[order->price] = std::make_shared<Dlist>(order->price);
                }
                sell_orders[order->price]->add_order(order);
                
                if (order_map.find(order->orderd_id) == order_map.end())
                {
                    order_map[order->orderd_id] = order;
                    total_sell_orders++;
                }
            }
            else
            {
                if (order_map.find(order->orderd_id) != order_map.end())
                {
                    order_map.erase(order->orderd_id);
                    total_sell_orders--;
                }
            }
        }
    }

public:
    Orderbook()
    {
        total_buy_orders = total_sell_orders = 0;
    }

    void place_order(size_t orderid, double price, size_t volume, Ordertype ordertype)
    {
        std::shared_ptr<Order> order = std::make_shared<Order>(orderid, price, volume, ordertype);
        process_order(order);
    }

    void cancel_order(size_t order_id)
    {
        if (order_map.find(order_id) == order_map.end())
        {
            return;
        }
        std::shared_ptr<Order> order = order_map[order_id];
        if (order->ordertype == Ordertype::BUY)
        {
            std::shared_ptr<Dlist> dlist = buy_orders[order->price];
            dlist->remove_order(order);
            if (dlist->size == 0)
            {
                buy_orders.erase(order->price);
            }
            total_buy_orders--;
        }
        else
        {
            std::shared_ptr<Dlist> dlist = sell_orders[order->price];
            dlist->remove_order(order);
            if (dlist->size == 0)
            {
                sell_orders.erase(order->price);
            }
            total_sell_orders--;
        }
        order_map.erase(order_id);
    }

    void update_order_by_price(size_t order_id, double new_price)
    {
        if (order_map.find(order_id) == order_map.end())
        {
            return;
        }
        std::shared_ptr<Order> order = order_map[order_id];
        if (order->ordertype == Ordertype::BUY)
        {
            buy_orders[order->price]->remove_order(order);
            if (buy_orders[order->price]->size == 0)
            {
                buy_orders.erase(order->price);
            }
        }
        else
        {
            sell_orders[order->price]->remove_order(order);
            if (sell_orders[order->price]->size == 0)
            {
                sell_orders.erase(order->price);
            }
        }
        order->update_price(new_price);
        process_order(order);
    }

    void update_order_by_volume(size_t order_id, size_t new_vol)
    {
        if (order_map.find(order_id) == order_map.end())
        {
            return;
        }
        std::shared_ptr<Order> order = order_map[order_id];
        if (order->ordertype == Ordertype::BUY)
        {
            buy_orders[order->price]->update_order_volume(order, new_vol);
        }
        else
        {
            sell_orders[order->price]->update_order_volume(order, new_vol);
        }
    }

    std::shared_ptr<Order> find_order(size_t order_id)
    {
        auto it = order_map.find(order_id);
        return it == order_map.end() ? nullptr : it->second;
    }

    size_t get_volume(double price, Ordertype ordertype)
    {
        if (ordertype == Ordertype::BUY)
        {
            auto it = buy_orders.find(price);
            return (it != buy_orders.end()) ? it->second->volume : 0;
        }
        else
        {
            auto it = sell_orders.find(price);
            return (it != sell_orders.end()) ? it->second->volume : 0;
        }
    }
};

}  // namespace legacy
