from dataclasses import dataclass

from . import orders

DEFAULT_CURRENCY = "EUR"
_registry_size = 0


@dataclass
class Product:
    sku: str
    name: str
    price: float

    def label(self) -> str:
        return f"{self.name} ({self.sku})"


class Catalog:
    def __init__(self):
        self.products: dict[str, Product] = {}

    def add(self, product: Product):
        global _registry_size
        _registry_size += 1
        self.products[product.sku] = product

    def find(self, sku: str) -> Product | None:
        return self.products.get(sku)

    def open_orders(self):
        # catalog reaching into orders: the two modules import each other
        return [o for o in orders.ALL_ORDERS if not o.closed]


def slugify(text):
    out = []
    word = ""
    for ch in text:
        if ch.isalnum():
            word += ch.lower()
        else:
            if word:
                out.append(word)
            word = ""
    if word:
        out.append(word)
    return "-".join(out)
