#include "screencapturer.h"
#include <QApplication>
#include <QScreen>
#include <QDesktopWidget>
#include <QFileDialog>
#include <QClipboard>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QDebug>

ScreenCapturer::ScreenCapturer(QWidget *parent)
    : Selector(parent)
{
    // menu
    menu_ = new MainMenu(this);
    menu_->hide();

    connect(menu_, &MainMenu::SAVE_IMAGE, this, &ScreenCapturer::save_image);
    connect(menu_, &MainMenu::COPY_TO_CLIPBOARD, this, &ScreenCapturer::copy2clipboard);
    connect(menu_, &MainMenu::FIX_IMAGE, this, &ScreenCapturer::fix_image);
    connect(menu_, &MainMenu::EXIT_CAPTURE, this, &ScreenCapturer::exit_capture);

    connect(menu_, &MainMenu::UNDO, this, &ScreenCapturer::undo);
    connect(menu_, &MainMenu::REDO, this, &ScreenCapturer::redo);
    connect(&undo_stack_, SIGNAL(changed()), this, SLOT(update()));
    connect(&undo_stack_, reinterpret_cast<void (CommandStack::*)(bool)>(&CommandStack::empty), [&](bool e) { status_ = e ? CAPTURED : LOCKED; });

    auto end_edit_functor = [=]() {
        if(undo_stack_.empty()){
            status_ = CAPTURED;
        }
        edit_status_ = NONE;

        gmenu_->hide();
        fmenu_->hide();
    };
    connect(menu_, &MainMenu::START_PAINT_RECTANGLE, [=]() { status_ = LOCKED;  edit_status_ = START_PAINTING_RECTANGLE; fmenu_->hide(); gmenu_->show(); });
    connect(menu_, &MainMenu::END_PAINT_RECTANGLE, end_edit_functor);

    connect(menu_, &MainMenu::START_PAINT_CIRCLE, [=]() { status_ = LOCKED;  edit_status_ = START_PAINTING_CIRCLE; fmenu_->hide(); gmenu_->show(); });
    connect(menu_, &MainMenu::END_PAINT_CIRCLE, end_edit_functor);

    connect(menu_, &MainMenu::START_PAINT_ARROW, [=]() { status_ = LOCKED;  edit_status_ = START_PAINTING_ARROW; fmenu_->hide(); gmenu_->show(); });
    connect(menu_, &MainMenu::END_PAINT_ARROW, end_edit_functor);

    connect(menu_, &MainMenu::START_PAINT_LINE, [=]() { status_ = LOCKED;  edit_status_ = START_PAINTING_LINE; fmenu_->hide(); gmenu_->show(); });
    connect(menu_, &MainMenu::END_PAINT_LINE, end_edit_functor);

    connect(menu_, &MainMenu::START_PAINT_CURVES, [=]() { status_ = LOCKED;  edit_status_ = START_PAINTING_CURVES; fmenu_->hide(); gmenu_->show(); });
    connect(menu_, &MainMenu::END_PAINT_CURVES, end_edit_functor);

    connect(menu_, &MainMenu::START_PAINT_TEXT, [=]() { status_ = LOCKED;  edit_status_ = START_PAINTING_TEXT; gmenu_->hide(); fmenu_->show(); });
    connect(menu_, &MainMenu::END_PAINT_TEXT, end_edit_functor);

    // graph menu
    gmenu_ = new GraphMenu(this);
    gmenu_->hide();

    connect(gmenu_, &GraphMenu::SET_WIDTH_01, [=](){ pen_width_ = 1; fill_ = false; });
    connect(gmenu_, &GraphMenu::SET_WIDTH_02, [=](){ pen_width_ = 2; fill_ = false; });
    connect(gmenu_, &GraphMenu::SET_WIDTH_03, [=](){ pen_width_ = 4; fill_ = false; });

    connect(gmenu_, &GraphMenu::SET_FILL, [=](){ fill_ = true; });
    connect(gmenu_, &GraphMenu::SET_UNFILL, [=](){ fill_ = false; });

    connect(gmenu_, &GraphMenu::SET_COLOR, [=](const QColor& color) { color_ = color; });

    // font menu
    fmenu_ = new FontMenu(this);
    fmenu_->hide();

    connect(fmenu_, &FontMenu::familyChanged, [=](const QString &family){ font_family_ = family; });
    connect(fmenu_, &FontMenu::styleChanged, [=](const QString &style){ font_style_ = style; });
    connect(fmenu_, &FontMenu::sizeChanged, [=](int s){ font_size_ = s; });
    connect(fmenu_, &FontMenu::colorChanged, [=](const QColor& color){ font_color_ = color; });

    // move menu
    auto menu_move_factor = [&]() {
        captured_screen_.height() - rb().y() <= menu_->height()
            ? menu_->move(rb().x() - menu_->width() + 1, rb().y() - menu_->height() - 1)
            : menu_->move(rb().x() - menu_->width() + 1, rb().y() + 3);

        gmenu_->move(rb().x() - menu_->width() + 1, rb().y() + menu_->height() + 5);
        fmenu_->move(rb().x() - menu_->width() + 1, rb().y() + menu_->height() + 5);
    };

    connect(this, &ScreenCapturer::moved, menu_move_factor);
    connect(this, &ScreenCapturer::resized, menu_move_factor);

    // Magnifier
    magnifier_ = new Magnifier(this);
    magnifier_->hide();
}

void ScreenCapturer::start()
{
    captured_screen_ = QGuiApplication::primaryScreen()->grabWindow(QApplication::desktop()->winId());

    Selector::start();

    menu_->hide();
    gmenu_->hide();
}

void ScreenCapturer::mousePressEvent(QMouseEvent *event)
{
    if(status_ == LOCKED) {
        switch (edit_status_) {
        case START_PAINTING_RECTANGLE:
        case END_PAINTING_RECTANGLE: rectangle_end_ = rectangle_begin_ = event->pos(); edit_status_ = PAINTING_RECTANGLE; break;
        case START_PAINTING_CIRCLE:
        case END_PAINTING_CIRCLE: circle_end_ = circle_begin_ = event->pos(); edit_status_ = PAINTING_CIRCLE; break;
        case START_PAINTING_ARROW:
        case END_PAINTING_ARROW: arrow_end_ = arrow_begin_ = event->pos(); edit_status_ = PAINTING_ARROW; break;
        case START_PAINTING_LINE:
        case END_PAINTING_LINE: line_end_ = line_begin_ = event->pos(); edit_status_ = PAINTING_LINE; break;
        case START_PAINTING_CURVES:
        case END_PAINTING_CURVES: curves_.push_back(event->pos()); edit_status_ = PAINTING_CURVES; break;
        case START_PAINTING_TEXT:
        case PAINTING_TEXT:
        case END_PAINTING_TEXT:
            edit_status_ = PAINTING_TEXT;
            text_pos_ = event->pos();

            if(!text_edit_ || !text_edit_->toPlainText().isEmpty()) {
                text_edit_ = new TextEdit(this);
            }
            text_edit_->setFocus();
            text_edit_->show();
            text_edit_->move(text_pos_);

            connect(text_edit_, &TextEdit::focus, [=](TextEdit* that) {
                return [=](bool f) {
                    if(!f && !that->toPlainText().isEmpty()) {
                        Command command(Command::DRAW_TEXT, QPen(font_color_));
                        command.widget(that);
                        DO(command);
                    }
                };
            }(text_edit_));
            break;
        default:
            break;
        }
    }

    Selector::mousePressEvent(event);
}

void ScreenCapturer::mouseMoveEvent(QMouseEvent* event)
{
    if(status_ == LOCKED) {
        switch (edit_status_) {
        case PAINTING_RECTANGLE: rectangle_end_ = event->pos(); setCursor(Qt::CrossCursor); break;
        case PAINTING_CIRCLE: circle_end_ = event->pos(); setCursor(Qt::CrossCursor); break;
        case PAINTING_ARROW: arrow_end_ = event->pos(); setCursor(Qt::CrossCursor); break;
        case PAINTING_LINE: line_end_ = event->pos(); setCursor(Qt::CrossCursor); break;
        case PAINTING_CURVES: curves_.push_back(event->pos()); setCursor(Qt::CrossCursor); break;
        case PAINTING_TEXT: setCursor(Qt::IBeamCursor); break;
        default:break;
        }
    }

    Selector::mouseMoveEvent(event);
}

void ScreenCapturer::mouseReleaseEvent(QMouseEvent *event)
{
    if(status_ == LOCKED) {
        switch (edit_status_) {
        case PAINTING_RECTANGLE:
        {
            DO(Command(Command::DRAW_RECTANGLE, QPen(color_, pen_width_, Qt::SolidLine), QPoint(), fill_, { rectangle_begin_, rectangle_end_ }));
            edit_status_ = END_PAINTING_RECTANGLE;

            break;
        }

        case PAINTING_CIRCLE:
        {

            DO(Command(Command::DRAW_CIRCLE, QPen(color_, pen_width_, Qt::SolidLine), QPoint(), fill_, { circle_begin_, circle_end_ }));
            edit_status_ = END_PAINTING_CIRCLE;

            break;
        }

        case PAINTING_ARROW:
        {
            DO(Command(Command::DRAW_ARROW, QPen(color_, 1, Qt::SolidLine), QPoint(), fill_, { arrow_begin_, arrow_end_ }));
            edit_status_ = END_PAINTING_ARROW;

            break;
        }

        case PAINTING_LINE:
        {
            DO(Command(Command::DRAW_BROKEN_LINE, QPen(color_, pen_width_, Qt::SolidLine), QPoint(), false, { line_begin_, line_end_ }));

            line_end_ = event->pos();
            edit_status_ = END_PAINTING_LINE;

            break;
        }

        case PAINTING_CURVES:
        {
            Command command(Command::DRAW_CURVE, QPen(color_, pen_width_, Qt::SolidLine, Qt::FlatCap));
            command.points(curves_);
            curves_.clear();
            DO(command);

            line_end_ = event->pos();
            edit_status_ = END_PAINTING_CURVES;
            break;
        }

        default: break;
        }
    }

    Selector::mouseReleaseEvent(event);
}

void ScreenCapturer::keyPressEvent(QKeyEvent *event)
{
    if((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && status_ >= CAPTURED) {
        copy2clipboard();
        exit_capture();
    }

    Selector::keyPressEvent(event);
}

void ScreenCapturer::updateMenuPosition()
{
    if(status_ <  CAPTURED) return;

    auto area = selected();

    menu_->show();

    menu_->height() * 2 + area.bottomRight().y() > rect().height()
        ? menu_->move(area.topRight().x() - menu_->width() + 1, area.topRight().y() + 1) // topright
        : menu_->move(area.bottomRight().x() - menu_->width() + 1, area.bottomRight().y() + 3); // bottomright

    gmenu_->move(menu_->pos().x(), menu_->pos().y() + menu_->height() + 1);
    fmenu_->move(menu_->pos().x(), menu_->pos().y() + menu_->height() + 1);
}

void ScreenCapturer::upadateMagnifierPosition()
{
    if(status_ != LOCKED) {
        auto mouse_pos = QCursor::pos();
        QRect rect(mouse_pos.x() - 10, mouse_pos.y() - 10, 21, 21);

        magnifier_->area(captured_screen_.copy(rect));
        magnifier_->show();
        magnifier_->move(QCursor::pos().x() + 1, QCursor::pos().y() + 1);
    }
    else {
        magnifier_->hide();;
    }
}

void ScreenCapturer::paintEvent(QPaintEvent *event)
{
    updateMenuPosition();
    upadateMagnifierPosition();

    QRect area = selected();
    auto background = captured_screen_.copy();

    painter_.begin(this);
    painter_.setRenderHint(QPainter::Antialiasing, false);


    if(status_ == LOCKED) {
        QPainter edit_painter;
        edit_painter.begin(&background);
        edit_painter.setPen(QPen(color_, 1, Qt::DashDotLine));

        //////////////////////////////////////////////////////////////////////////////////////////////////////
        // track
        switch(edit_status_) {
        case PAINTING_RECTANGLE:
            edit_painter.drawRect(QRect(rectangle_begin_, rectangle_end_));
            break;

        case PAINTING_CIRCLE:
            edit_painter.setRenderHint(QPainter::Antialiasing, true);
            edit_painter.drawEllipse(QRect(circle_begin_, circle_end_));
            break;

        case PAINTING_ARROW:
        {
            QPoint points[6];
            getArrowPoints(arrow_begin_, arrow_end_, points);
            edit_painter.drawPolygon(points, 6);

            break;
        }

        case PAINTING_LINE:
            edit_painter.drawLine(line_begin_, line_end_);
            break;

        case PAINTING_CURVES:
            edit_painter.setRenderHint(QPainter::Antialiasing, true);
            for(size_t i = 0; i < curves_.size() - 1; ++i) {
                edit_painter.drawLine(curves_[i], curves_[i + 1]);
            }
            break;

        case PAINTING_TEXT:
        {
            QFont font;
            font.setFamily(font_family_);
            font.setStyleName(font_style_);
            font.setPointSize(font_size_);
            text_edit_->setTextColor(font_color_);
            text_edit_->setFont(font);

            QRect text_rect(text_pos_, text_edit_->size());
            edit_painter.drawRect(text_rect);

            break;
        }

        default: break;
        }

        //////////////////////////////////////////////////////////////////////////////////////////////////////
        // final
        for(auto& command: undo_stack_.commands()) {
            edit_painter.setPen(command.pen());
            edit_painter.setBrush(Qt::NoBrush);

            switch (command.type()) {
            case Command::DRAW_RECTANGLE:
                edit_painter.setRenderHint(QPainter::Antialiasing, false);
                if(command.isFill()) edit_painter.setBrush(command.pen().color());

                 edit_painter.drawRect(QRect(command.points()[0], command.points()[1]));
                break;

            case Command::DRAW_CIRCLE:
                edit_painter.setRenderHint(QPainter::Antialiasing, true);
                if(command.isFill()) edit_painter.setBrush(command.pen().color());

                 edit_painter.drawEllipse(QRect(command.points()[0], command.points()[1]));
                break;

            case Command::DRAW_BROKEN_LINE:
                edit_painter.setRenderHint(QPainter::Antialiasing, true);
                edit_painter.drawLine(command.points()[0], command.points()[1]);
                break;

            case Command::DRAW_ARROW:
            {
                edit_painter.setRenderHint(QPainter::Antialiasing, true);
                QPoint points[6];
                getArrowPoints(command.points()[0], command.points()[1], points);
                edit_painter.setBrush(command.pen().color());
                edit_painter.drawPolygon(points, 6);

                break;
            }

            case Command::DRAW_CURVE:
            {
                edit_painter.setRenderHint(QPainter::Antialiasing, true);
                auto points = command.points();
                for(size_t i = 0; i < points.size() - 1; ++i) {
                    edit_painter.drawLine(points[i], points[i + 1]);
                }

                break;
            }

            case Command::DRAW_TEXT:
            {
                edit_painter.setRenderHint(QPainter::Antialiasing, false);
                auto edit = reinterpret_cast<TextEdit*>(command.widget());
                edit_painter.setFont(edit->font());
                edit_painter.drawText(edit->geometry() + QMargins(-4, -4, 0, 0), Qt::TextWordWrap, edit->toPlainText());
                break;
            }

            default: break;
            }
        }
    }

    painter_.drawPixmap(0, 0, background);
    painter_.fillRect(background.rect(), QColor(0, 0, 0, 100));
    captured_image_ = background.copy(area);
    painter_.drawPixmap(area.topLeft(), captured_image_);

    painter_.end();
    Selector::paintEvent(event);
}

void ScreenCapturer::getArrowPoints(QPoint begin, QPoint end, QPoint* points)
{
    double par = 10.0;
    double par2 = 27.0;
    double slopy = atan2((end.y() - begin.y()), (end.x() - begin.x()));
    double alpha = 30 * 3.14 / 180;

    points[1] = QPoint(end.x() - int(par * cos(alpha + slopy)) - int(9 * cos(slopy)), end.y() - int(par*sin(alpha + slopy)) - int(9 * sin(slopy)));
    points[5] = QPoint(end.x() - int(par * cos(alpha - slopy)) - int(9 * cos(slopy)), end.y() + int(par*sin(alpha - slopy)) - int(9 * sin(slopy)));

    points[2] = QPoint(end.x() - int(par2 * cos(alpha + slopy)), end.y() - int(par2*sin(alpha + slopy)));
    points[4] = QPoint(end.x() - int(par2 * cos(alpha - slopy)), end.y() + int(par2*sin(alpha - slopy)));

    points[0] = begin;
    points[3] = end;
}


void ScreenCapturer::save_image()
{
    auto filename = QFileDialog::getSaveFileName(this,
                                                 tr("Save Image"),
                                                 QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
                                                 tr("Image Files (*.png *.jpeg *.jpg *.bmp)"));

    if(!filename.isEmpty()) {
        captured_image_.save(filename);
        CAPTURE_SCREEN_DONE(captured_image_);
        exit_capture();
    }
}

void ScreenCapturer::copy2clipboard()
{
    QApplication::clipboard()->setPixmap(captured_image_);
    CAPTURE_SCREEN_DONE(captured_image_);
    exit_capture();
}

void ScreenCapturer::fix_image()
{
    FIX_IMAGE(captured_image_);
    CAPTURE_SCREEN_DONE(captured_image_);
    exit_capture();
}

void ScreenCapturer::exit_capture()
{
    status_ =  INITIAL;
    end_ = begin_ = { 0, 0 };

    undo_stack_.clear();

    menu_->hide();
    gmenu_->hide();
    fmenu_->hide();
    hide();
}

///////////////////////////////////////////////////// UNDO / REDO ////////////////////////////////////////////////////////////

void ScreenCapturer::undo()
{
    if(undo_stack_.empty()) return;

    redo_stack_.push(undo_stack_.back());
    undo_stack_.pop();
}

void ScreenCapturer::redo()
{
    if(redo_stack_.empty()) return;

    undo_stack_.push(redo_stack_.back());
    redo_stack_.pop();
}