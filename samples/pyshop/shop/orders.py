from .catalog import Catalog, Product

ALL_ORDERS = []


class Base:
    def total(self):
        raise NotImplementedError

    def describe(self):
        return "an order of " + str(self.total())


class Order(Base):
    def __init__(self, catalog: Catalog):
        self.catalog = catalog
        self.lines = []
        self.closed = False
        self.audit = 0
        ALL_ORDERS.append(self)

    def add(self, sku, quantity=1):
        product = self.catalog.find(sku)
        if product is None:
            raise KeyError(sku)
        self.lines.append((product, quantity))
        self.audit = len(self.lines)

    def total(self):
        return sum(p.price * q for p, q in self.lines)

    def summary(self, verbose, currency, discount, rounding, show_empty, limit):
        out = []
        for i, (product, quantity) in enumerate(self.lines):
            if limit and i >= limit:
                break
            if verbose:
                if quantity > 10 and discount:
                    out.append(f"{product.label()} x{quantity} (bulk)")
                elif quantity == 0 and not show_empty:
                    continue
                else:
                    out.append(f"{product.label()} x{quantity}")
            else:
                out.append(product.sku)
        total = self.total() * (1 - discount if discount else 1)
        if rounding:
            total = round(total, 2)
        return "\n".join(out) + f"\n{total} {currency}"


def make_slug(text):
    out = []
    part = ""
    for c in text:
        if c.isalnum():
            part += c.lower()
        else:
            if part:
                out.append(part)
            part = ""
    if part:
        out.append(part)
    return "-".join(out)


def legacy_total(order):
    return order.total()


def main():
    catalog = Catalog()
    catalog.add(Product("a-1", "Apple", 0.5))
    order = Order(catalog)
    order.add("a-1", 3)
    print(order.summary(True, "EUR", 0, True, False, 0))
    print(make_slug("Hello World"))


if __name__ == "__main__":
    main()
