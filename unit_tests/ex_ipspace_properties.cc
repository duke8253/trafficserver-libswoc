// SPDX-License-Identifier: Apache-2.0
// Copyright 2014 Network Geographics

/** @file

    Example use of IPSpace for property mapping.
*/

#include "catch.hpp"

#include <memory>
#include <limits>

#include <swoc/TextView.h>
#include <swoc/swoc_ip.h>
#include <swoc/bwf_ip.h>
#include <swoc/swoc_file.h>

using namespace std::literals;
using namespace swoc::literals;
using swoc::TextView;
using swoc::IPEndpoint;

using swoc::IP4Addr;
using swoc::IP4Range;

using swoc::IP6Addr;
using swoc::IP6Range;

using swoc::IPAddr;
using swoc::IPRange;
using swoc::IPSpace;

using swoc::MemSpan;
using swoc::MemArena;

using W = swoc::LocalBufferWriter<256>;

// ---

/** A "table" is conceptually a table with the rows labeled by IP address and a set of
 * property columns that represent data for each IP address.
 */
class Table {
  using self_type = Table; ///< Self reference type.
public:
  static constexpr char SEP = ','; /// Value separator for input file.

  /** A property is the description of data for an address.
   * The table consists of an ordered list of properties, each corresponding to a column.
   */
  class Property {
    using self_type = Property; ///< Self reference type.
  public:
    /// A handle to an instance.
    using Handle = std::unique_ptr<self_type>;

    /** Construct an instance.
     *
     * @param name Property name.
     */
    Property(TextView const& name) : _name(name) {};

    /// Force virtual destructor.
    virtual ~Property() = default;

    /** The size of the property in bytes.
     *
     * @return The amount of data needed for a single instance of the property value.
     */
    virtual size_t size() const = 0;

    /** The index in the table of the property.
     *
     * @return The column index.
     */
    unsigned idx() const { return _idx; }

    /** Token persistence.
     *
     * @return @c true if the token needs to be preserved, @c false if not.
     *
     * If the token for the value is consumed, this should be left as is. However, if the token
     * itself needs to be persistent for the lifetime of the table, this must be overridden to
     * return @c true.
     */
    virtual bool needs_localized_token() const { return false; }

    /// @return The row data offset in bytes for this property.
    size_t offset() const { return _offset; }

    /** Parse the @a token.
     *
     * @param token Value from the input file for this property.
     * @param span Row data storage for this property.
     * @return @c true if @a token was correctly parse, @c false if not.
     *
     * The table parses the input file and handles the fields in a line. Each value is passed to
     * the corresponding property for parsing via this method. The method should update the data
     * pointed at by @a span.
     */
    virtual bool parse(TextView token, MemSpan<std::byte> span) = 0;

  protected:
    friend class Table;

    TextView _name; ///< Name of the property.
    unsigned _idx = std::numeric_limits<unsigned>::max(); ///< Column index.
    size_t _offset = std::numeric_limits<size_t>::max(); ///< Offset into a row.

    /** Set the column index.
     *
     * @param idx Index for this property.
     * @return @a this.
     *
     * This is called from @c Table to indicate the column index.
     */
    self_type & assign_idx(unsigned idx) { _idx = idx; return *this; }

    /** Set the row data @a offset.
     *
     * @param offset Offset in bytes.
     * @return @a this
     *
     * This is called from @c Table to store the row data offset.
     */
    self_type & assign_offset(size_t offset) { _offset = offset; return *this; }
  };

  /// Construct an empty Table.
  Table() = default;

  /** Add a property column to the table.
   *
   * @param col Column descriptor.
   * @return @a this
   */
  self_type & add_column(Property::Handle && col);

  /// A row for the table.
  class Row {
    using self_type = Row;
  public:
    Row(MemSpan<std::byte> span) : _data(span) {}
    MemSpan<std::byte> span_for(Property const& prop) const {
      return MemSpan<std::byte>{_data}.remove_prefix(prop.offset());
    }
  protected:
    MemSpan<std::byte> _data;
  };

  /** Parse input.
   *
   * @param src The source to parse.
   * @return @a true if parsing was successful, @c false if not.
   *
   * In general, @a src will be the contents of a file.
   *
   * @see swoc::file::load
   */
  bool parse(TextView src);

  /** Look up @a addr in the table.
   *
   * @param addr Address to find.
   * @return A @c Row for the address, or @c nullptr if not found.
   */
  Row* find(IPAddr const& addr);

  /// @return The number of ranges in the container.
  size_t size() const { return _space.count(); }

  /** Property for column @a idx.
   *
   * @param idx Index.
   * @return The property.
   */
  Property * column(unsigned idx) { return _columns[idx].get(); }

protected:
  size_t _size = 0; ///< Size of row data.
  /// Defined properties for columns.
  std::vector<Property::Handle> _columns;

  /// IPSpace type.
  using space = IPSpace<Row>;
  space _space; ///< IPSpace instance.

  MemArena _arena; ///< Arena for storing rows.

  /** Extract the next token from the line.
   *
   * @param line Current line [in,out]
   * @return Extracted token.
   */
  TextView token(TextView & line);

  TextView localize(TextView const& src);
};

auto Table::add_column(Property::Handle &&col) -> self_type & {
  col->assign_offset(_size);
  col->assign_idx(_columns.size());
  _size += col->size();
  _columns.emplace_back(std::move(col));
  return *this;
}

TextView Table::localize(TextView const&src) {
  auto span = _arena.alloc(src.size()).rebind<char>();
  memcpy(span, src);
  return span.view();
}

TextView Table::token(TextView & line) {
  TextView::size_type idx = 0;
  // Characters of interest in a null terminated string.
  char sep_list[3] = {'"', SEP, 0};
  bool in_quote_p  = false;
  while (idx < line.size()) {
    // Next character of interest.
    idx = line.find_first_of(sep_list, idx);
    if (TextView::npos == idx) {
      // no more, consume all of @a line.
      break;
    } else if ('"' == line[idx]) {
      // quote, skip it and flip the quote state.
      in_quote_p = !in_quote_p;
      ++idx;
    } else if (SEP == line[idx]) { // separator.
      if (in_quote_p) {
        // quoted separator, skip and continue.
        ++idx;
      } else {
        // found token, finish up.
        break;
      }
    }
  }

  // clip the token from @a src and trim whitespace, quotes
  auto zret = line.take_prefix(idx).trim_if(&isspace).trim('"');
  return zret;
}

bool Table::parse(TextView src) {
  unsigned line_no = 0;
  while (src) {
    auto line = src.take_prefix_at('\n');
    ++line_no;
    auto range_token = line.take_prefix_at(',');
    IPRange range{range_token};
    if (range.empty()) {
      std::cout << W().print("{} is not a valid range specification.", range_token);
      continue; // This is an error, real code should report it.
    }
    MemSpan<std::byte> span = _arena.alloc(_size).rebind<std::byte>();
    Row row{span};
    for ( auto const& col : _columns) {
      auto token = this->token(line);
      if (col->needs_localized_token()) {
        token = this->localize(token);
      }
      if (! col->parse(token, MemSpan<std::byte>{span.data(), col->size()})) {
        std::cout << W().print("Value \"{}\" at index {} on line {} is invalid.", token, col->idx(), line_no);
      }
      span.remove_prefix(col->size());
    }
    _space.mark(range, std::move(row));
  }
  return true;
}

auto Table::find(IPAddr const &addr) -> Row * {
  return _space.find(addr);
}

bool operator == (Table::Row const&, Table::Row const&) { return false; }

// ---

class FlagProperty : public Table::Property {
  using self_type = FlagProperty;
  using super_type = Table::Property;
public:
  static constexpr size_t SIZE = sizeof(bool);
protected:
  size_t size() const override { return SIZE; }
  bool parse(TextView token, MemSpan<std::byte> span) override;
};

class FlagGroupProperty : public Table::Property {
  using self_type = FlagGroupProperty;
  using super_type = Table::Property;
public:
  static constexpr size_t SIZE = sizeof(uint8_t);
  FlagGroupProperty(TextView const& name, std::initializer_list<TextView> tags);

  bool is_set(unsigned idx, Table::Row const& row) const;
protected:
  size_t size() const override { return SIZE; }
  bool parse(TextView token, MemSpan<std::byte> span) override;
  std::vector<TextView> _tags;
};

class TagProperty : public Table::Property {
  using self_type = TagProperty;
  using super_type = Table::Property;
public: // owner
  static constexpr size_t SIZE = sizeof(uint8_t);
  using super_type::super_type;
protected:
  std::vector<TextView> _tags;

  size_t size() const override { return SIZE; }
  bool parse(TextView token, MemSpan<std::byte> span) override;
};

class StringProperty : public Table::Property {
  using self_type = StringProperty;
  using super_type = Table::Property;
public:
  static constexpr size_t SIZE = sizeof(TextView);
  using super_type::super_type;

protected:
  size_t size() const override { return SIZE; }
  bool parse(TextView token, MemSpan<std::byte> span) override;
  bool needs_localized_token() const { return true; }
};

// ---
bool StringProperty::parse(TextView token, MemSpan<std::byte> span) {
  memcpy(span.data(), &token, sizeof(token));
  return true;
}

FlagGroupProperty::FlagGroupProperty(TextView const& name, std::initializer_list<TextView> tags)
    : super_type(name)
{
  _tags.reserve(tags.size());
  for ( auto const& tag : tags ) {
    _tags.emplace_back(tag);
  }
}

bool FlagGroupProperty::parse(TextView token, MemSpan<std::byte> span) {
  if ("-"_tv == token) { return true; } // marker for no flags.
  memset(span, 0);
  while (token) {
    auto tag = token.take_prefix_at(';');
    unsigned j = 0;
    for ( auto const& key : _tags ) {
      if (0 == strcasecmp(key, tag)) {
        span[j/8] |= (std::byte{1} << (j % 8));
        break;
      }
      ++j;
    }
    if (j > _tags.size()) {
      std::cout << W().print("Tag \"{}\" is not recognized.", tag);
      return false;
    }
  }
  return true;
}

bool FlagGroupProperty::is_set(unsigned flag_idx, Table::Row const& row) const {
  auto sp = row.span_for(*this);
  return std::byte{0} != ((sp[flag_idx/8] >> (flag_idx%8)) & std::byte{1});
}

bool TagProperty::parse(TextView token, MemSpan<std::byte> span) {
  // Already got one?
  auto spot = std::find_if(_tags.begin(), _tags.end(), [&](TextView const& tag) { return 0 == strcasecmp(token, tag); });
  if (spot == _tags.end()) { // nope, add it to the list.
    _tags.push_back(token);
    spot = std::prev(_tags.end());
  }
  span.rebind<uint8_t>()[0] = spot - _tags.begin();
  return true;
}

// ---

TEST_CASE("IPSpace properties", "[libswoc][ip][ex][properties]") {
  Table table;
  auto flag_names = { "prod"_tv, "dmz"_tv, "internal"_tv};
  table.add_column(std::make_unique<TagProperty>("owner"));
  table.add_column(std::make_unique<TagProperty>("colo"));
  table.add_column(std::make_unique<FlagGroupProperty>("flags"_tv, flag_names));
  table.add_column(std::make_unique<StringProperty>("Description"));

  TextView src = R"(10.1.1.0/24,asf,cmi,prod;internal,"ASF core net"
192.168.28.0/25,asf,ind,prod,"Indy Net"
192.168.28.128/25,asf,abq,dmz;internal,"Albuquerque zone"
)";

  REQUIRE(true == table.parse(src));
  REQUIRE(3 == table.size());
  auto row = table.find(IPAddr{"10.1.1.56"});
  REQUIRE(nullptr != row);
  CHECK(true == static_cast<FlagGroupProperty*>(table.column(2))->is_set(0, *row));
  CHECK(false == static_cast<FlagGroupProperty*>(table.column(2))->is_set(1, *row));
  CHECK(true == static_cast<FlagGroupProperty*>(table.column(2))->is_set(2, *row));
};

