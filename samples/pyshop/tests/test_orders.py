from shop.catalog import Catalog, Product
from shop.orders import Order


def test_total():
    catalog = Catalog()
    catalog.add(Product("x", "X", 2.0))
    order = Order(catalog)
    order.add("x", 2)
    assert order.total() == 4.0
